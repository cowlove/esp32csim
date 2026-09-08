#ifdef CSIM
#include "esp32csim.h"
#include <assert.h>

// TODO move failure modes into esp32csim.h 
//#include "jimlib.h"

void setup(void);
void loop(void);
uint64_t _micros = 0;
uint64_t _microsMax = 0xffffffff;
static void csim_save_rtc_mem();

static const int MAX_SEMAPHORES = 32;
int Semaphores[MAX_SEMAPHORES];
int nextSem = 0;
int xSemaphoreCreateCounting(int max, int init) { 
	Semaphores[nextSem] = init;
	assert(nextSem < MAX_SEMAPHORES);
	return nextSem++;
} 
int xSemaphoreCreateMutex() { return xSemaphoreCreateCounting(1, 1); } 
int xSemaphoreGive(int h) { Semaphores[h]++; return 0; } 
int xSemaphoreTake(int h, int delay) {
	if (Semaphores[h] > 0) {
		Semaphores[h]--;
		return 1;
	}
	return 0;
}
int uxSemaphoreGetCount(int h) { return Semaphores[h]; }

// TODO move this to pinManager
int Csim_currentPwm[16];
void ledcWrite(int chan, int val) {
		Csim_currentPwm[chan] = val;
} 

#define SIMFAILURE(x) (sim().simFailureHook(x))

Csim_pinManager &Csim_pins() { 
	static Csim_pinManager *firstUse = new Csim_pinManager();
	return *firstUse;
} 

void esp_read_mac(uint8_t *out, int) {
	uint64_t nmac = htonll(currentContext->mac);
	uint8_t *p = (uint8_t *)&nmac;
	memcpy(out, p + 2, 6);
}

FakeSerial Serial, Serial1, Serial2;
InterruptManager intMan;
FakeESP ESP;
FakeArduinoOTA ArduinoOTA;
FakeSPIFFS SPIFFS, LittleFS;
FakeWiFi WiFi;
FakeSD SD;
FakeWire Wire;
Update_t Update;
FakeCAN CAN;

Csim &sim() { 
	static Csim *staticFirstUse = new Csim();
	return *staticFirstUse;
}

CsimContext defaultContext = {0xffeeddaabbcc, NULL};
CsimContext *currentContext = &defaultContext;

void Csim_exit() { 	sim().exit(); }

uint64_t sleep_timer = 0;
vector<deepSleepHookT> deepSleepHooks;
void csim_onDeepSleep(deepSleepHookT func) { deepSleepHooks.push_back(func); }

int esp_sleep_enable_timer_wakeup(uint64_t t) {
	sleep_timer = t; // Retain the legacy process-global value for old callers.
	currentContext->sleepTimerUsec = t;
	return 0;
}

static vector<CsimContext *> privateContexts() {
	vector<CsimContext *> contexts;
	for (auto module : sim().modules) {
		CsimContext *context = module->context;
		if (context == NULL || context == &defaultContext)
			continue;
		if (std::find(contexts.begin(), contexts.end(), context) == contexts.end())
			contexts.push_back(context);
	}
	return contexts;
}

class CsimScopedContextSwap {
	CsimContext *origC;
public:
	CsimScopedContextSwap(CsimContext *c) {
		origC = currentContext;
		currentContext = c == NULL ? &defaultContext : c;
	}
	~CsimScopedContextSwap() {
		if (origC != NULL)
			currentContext = origC;
	}
};

static const char *contextSleepStateFile = "./csim_context_sleep.txt";
static void reexecAfterDeepSleep(uint64_t waitUsec);

static void saveContextSleepState() {
	FILE *f = fopen(contextSleepStateFile, "w");
	if (f == NULL)
		return;
	for (auto context : privateContexts())
		if (context->sleeping)
			fprintf(f, "%012llx %llu\n", (unsigned long long)context->mac,
					(unsigned long long)context->wakeTimeUsec);
	fclose(f);
}

static void loadContextSleepState() {
	FILE *f = fopen(contextSleepStateFile, "r");
	if (f == NULL)
		return;
	unsigned long long mac, wake;
	while (fscanf(f, "%llx %llu", &mac, &wake) == 2) {
		for (auto context : privateContexts()) {
			if (context->mac != mac)
				continue;
			context->wakeTimeUsec = wake;
			context->sleeping = wake > sim().bootTimeUsec;
			if (!context->sleeping) {
				context->wakeTimeUsec = 0;
				// This context woke as part of process startup.  A later wake
				// in this same process run must force another re-exec.
				context->wokeThisRun = true;
			}
		}
	}
	fclose(f);
}

// Context wakeup is a live scheduler event.  A context whose deadline passes
// while another context is running becomes runnable in the same process; it
// does not need to wait for the next process re-exec.
static void wakeDueContexts() {
	uint64_t now = sim().bootTimeUsec + _micros;
	for (auto module : sim().modules) {
		CsimContext *context = module->context;
		if (context == NULL || context == &defaultContext || !context->sleeping)
			continue;
		if (context->wakeTimeUsec > now)
			continue;
		if (context->wokeThisRun) {
			// Do not dispatch a second boot for this context in the same
			// process execution.  Re-exec at the current simulated time so
			// normal global/static initialization occurs before it runs again.
			reexecAfterDeepSleep(0);
			return;
		}
		context->sleeping = false;
		context->wakeTimeUsec = 0;
		context->wokeThisRun = true;
		CsimScopedContextSwap swap(context);
		module->setup();
	}
}

static void reexecAfterDeepSleep(uint64_t waitUsec) {
	if (sim().stopTimeUsec != 0 &&
		sim().bootTimeUsec + _micros + waitUsec >= sim().stopTimeUsec) {
		fflush(stdout);
		Csim_exit();
	}

	for (auto i : deepSleepHooks) i(waitUsec);
	saveContextSleepState();
	csim_save_rtc_mem(); 

	char *argv[128];
	int argc = 0; 
	// strip out all --boot-time, --seconds, --reset-reason and --show-args command line arguments
	for(char *const *p = sim().argv; *p != NULL; p++) {
		if (strcmp(*p, "--boot-time") == 0) p++;
		else if (strcmp(*p, "--seconds") == 0) p++;
		else if (strcmp(*p, "-s") == 0) p++;
		else if (strcmp(*p, "--stop-time-usec") == 0) p++;
		else if (strcmp(*p, "--reset-reason") == 0) p++;
		else if (strcmp(*p, "--show-args") == 0) {/*skip arg*/}
		else argv[argc++] = *p;
	}
	char bootTimeBuf[32], stopTimeBuf[32];
	snprintf(bootTimeBuf, sizeof(bootTimeBuf), "%ld", sim().bootTimeUsec + waitUsec + _micros);
	argv[argc++] = (char *)"--boot-time";
	argv[argc++] = bootTimeBuf; 
	snprintf(stopTimeBuf, sizeof(stopTimeBuf), "%llu",
			(unsigned long long)sim().stopTimeUsec);
	argv[argc++] = (char *)"--stop-time-usec";
	argv[argc++] = stopTimeBuf;
	argv[argc++] = (char *)"--reset-reason";
	argv[argc++] = (char *)"5";
	argv[argc++] = (char *)"--show-args";
	argv[argc++] = NULL;
	fflush(stdout);
	// Re-execute the same simulator binary that was launched.  Harnesses such
	// as csimix are not necessarily named "csim"; using a hard-coded path can
	// silently switch to a stale executable on the first deep-sleep reboot.
	execv(sim().argv[0], argv);
}

void esp_deep_sleep_start() {
	auto contexts = privateContexts();
	if (contexts.empty()) {
		reexecAfterDeepSleep(currentContext->sleepTimerUsec != 0
				? currentContext->sleepTimerUsec : sleep_timer);
		return;
	}

	assert(currentContext != &defaultContext);
	wakeDueContexts();
	currentContext->sleeping = true;
	currentContext->wakeTimeUsec = sim().bootTimeUsec + _micros
			+ currentContext->sleepTimerUsec;
	saveContextSleepState();

	uint64_t earliestWake = UINT64_MAX;
	for (auto context : contexts) {
		if (!context->sleeping)
			return;
		earliestWake = min(earliestWake, context->wakeTimeUsec);
	}
	uint64_t now = sim().bootTimeUsec + _micros;
	reexecAfterDeepSleep(earliestWake > now ? earliestWake - now : 0);
}
void esp_light_sleep_start() {
	delayMicroseconds(0); // run csim hooks 
	_micros += currentContext->sleepTimerUsec != 0
			? currentContext->sleepTimerUsec : sleep_timer;
	if (sim().stopTimeUsec != 0 && sim().bootTimeUsec + micros() > sim().stopTimeUsec)
		Csim_exit();
} 

void esp_wifi_set_promiscuous_rx_cb(void (*f)(void *, wifi_promiscuous_pkt_type_t)) {
	// HACK: just do a few immediate dummy callbacks to test codepath 
	typedef struct {
		unsigned protocol:2;
		unsigned type:2;
		unsigned subtype:4;
		unsigned ignore1:24;
		unsigned long long recv_addr:48; 
		unsigned long long send_addr:48; 
		unsigned ignore2:16;
		uint64_t timestamp;
	} raw_beacon_packet_t;    
	
    for(int n = 0; n < 10; n++) { 
        wifi_promiscuous_pkt_t pt = {0};
        raw_beacon_packet_t pk = {0};
        pt.payload = (uint8_t *)&pk;
        pk.subtype = 0x8;
        pk.timestamp = 0x1234123412341234LL + n * 1000;
        pt.rx_ctrl.rssi = -76;
		for (int m = 0; m < 5; m++) {
			pk.send_addr = 0xfeedfeedfee0LL + m;
			f((void *)&pt, 0);
		}
    }
}

static int csim_defaultOnPOST(const char *url, const char *hdr, const char *data, string &result) {
	string cmd = string("curl --silent -X POST -H '") + hdr + "' -d '" +
		data + "' " + url;
	FILE *fp = popen(cmd.c_str(), "r");
	int bufsz = 64 * 1024;
	char *buf = (char *)malloc(bufsz);
	result = fgets(buf, bufsz, fp);
	result = buf;
	fclose(fp);
	free(buf);
	return 200;
}

static int csim_defaultOnGET(const char *url, const char *hdr, const char *data, string &result) {
	string cmd = "curl --silent -X GET '" + string(url) + "'";
	FILE *fp = popen(cmd.c_str(), "r");
	int bufsz = 128 * 1024;
	char *buf = (char *)malloc(bufsz);
	int n = fread(buf, 1, bufsz, fp);
	result.assign(buf, n);
	fclose(fp);
	free(buf);
	return 200;
}

vector<HTTPClient::postHookInfo> &HTTPClient::csim_hooks() { 
	static vector<postHookInfo> *firstUse = new vector<postHookInfo>({
		{".*", true, csim_defaultOnPOST },
		{".*", false, csim_defaultOnGET }});
	return *firstUse;
}

int HTTPClient::csim_doPOSTorGET(bool isPost, const char *data, string &result) { 
	string hdr = header1 + ": " + header2;
	auto p = csim_hooks().end();
	std::cmatch m;
	for(auto i  = csim_hooks().begin(); i != csim_hooks().end(); i++) { // find best hook
		if (std::regex_match(url.c_str(), m, std::regex(i->urlRegex))) {
			if (i->urlRegex.length() > p->urlRegex.length() && i->isPost == isPost) {
				p = i;
			}
		}
	}
	if (p != csim_hooks().end()) 
		return p->func(url.c_str(), hdr.c_str(), data, result);
	return -1;
}

//vector<HTTPClient::postHookInfo> HTTPClient::csim_hooks = {
//	{".*", true, csim_defaultOnPOST },
//	{".*", false, csim_defaultOnGET }};

DHT::CsimInterface &DHT::csim() { 
	static CsimInterface *firstUse = new CsimInterface(); 
	return *firstUse; 
}

// TODO: extend this to use vector<unsigned char> to handle binary data	
WiFiUDP::InputMap WiFiUDP::inputMap;

vector<ESPNOW_csimOneProg *> &ESPNOW_csimOneProg::instanceList() { 
	static vector<ESPNOW_csimOneProg *> firstUse;
	return firstUse;
}

void ESPNOW_csimOneProg::send(const uint8_t *mac_addr, const uint8_t *data, int data_len) {//override {
	SimPacket p; 
	uint8_t mymac[6];
	esp_read_mac(mymac, sizeof(mymac));
	memcpy(p.send_addr, mymac, sizeof(p.send_addr));
	memcpy(p.recv_addr, mac_addr, sizeof(p.recv_addr));
	const char *cp = (const char *)data;
	p.data = string(cp, cp + data_len);
	for(auto o : instanceList()) {
		// A sleeping simulated device has its radio powered down.  Do not
		// queue packets for later delivery after it wakes; on real hardware
		// those over-the-air packets were simply missed.
		if (o != this && (o->context == NULL || !o->context->sleeping))
			o->pktQueue.push_back(p);
	}
}

void ESPNOW_csimOneProg::loop() { // override
	for(auto pkt : pktQueue) {
		if (recv_cb != NULL) {
			uint64_t sender = 0;
			for (uint8_t octet : pkt.send_addr) sender = (sender << 8) | octet;
			const uint64_t receiver = context ? context->mac : 0;
			if (SIMFAILURE("espnow-rx") || SIMFAILURE("espnow"))
				continue;
			if (sim().espnowDeliveryFailureHook(sender, receiver))
				continue;
			recv_cb(pkt.send_addr, (const uint8_t *)pkt.data.c_str(), pkt.data.length());
		}
	}
	pktQueue.clear();
} 

// Higher fidelity ESPNOW simulation between two sketch processes using named pipes
//   example:
//   ./csim --espnowPipe /tmp/fifo2 /tmp/fifo1 --mac fff1
//   ./csim --espnowPipe /tmp/fifo1 /tmp/fifo2 --mac fff2

class ESPNOW_csimPipe : public Csim_Module, public ESPNOW_csimInterface {
	int fdIn;
	const char *outFilename;
public:
	ESPNOW_csimPipe(const char *inFile, const char *outF);
	void send(const uint8_t *mac_addr, const uint8_t *data, int len) override;
	void loop() override;
};



ESPNOW_csimPipe::ESPNOW_csimPipe(const char *inFile, const char *outF) : outFilename(outF) { 
	fdIn = open(inFile, O_RDONLY | O_NONBLOCK); 
}

void ESPNOW_csimPipe::send(const uint8_t *mac_addr, const uint8_t *data, int len) { //override
	int fdOut = open(outFilename, O_WRONLY | O_NONBLOCK); 
	if(fdOut > 0) {
		int n = ::write(fdOut, data, len);
		if (n <= 0) { 
			printf("write returned %d", n);

		}
	}
	close(fdOut);
}

void ESPNOW_csimPipe::loop() { // override
	static uint8_t mac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	char buf[8192];
	int n;
	while(fdIn > 0 && (n = ::read(fdIn, buf, sizeof(buf))) > 0) { 
		if (SIMFAILURE("espnow-rx") || SIMFAILURE("espnow"))
		return;

		if (recv_cb != NULL) {
			recv_cb(mac, (uint8_t *)buf, n);
		}	
	}
} 

int esp_now_send(const uint8_t*mac, const uint8_t*data, size_t len) {
	if (SIMFAILURE("espnow-tx") || SIMFAILURE("espnow"))
		return ESP_OK;
	if (currentContext->espnow != NULL) {
		currentContext->espnow->send(mac, data, len);
		if (currentContext->espnow->send_cb != NULL) 
			currentContext->espnow->send_cb(mac, ESP_NOW_SEND_SUCCESS);
	}
	return ESP_OK; 
}

int esp_now_register_send_cb(esp_now_send_cb_t cb) { 
	if (currentContext->espnow != NULL) currentContext->espnow->send_cb = cb; 
	return ESP_OK; 
}
int esp_now_register_recv_cb(esp_now_recv_cb_t cb) { 
	if (currentContext->espnow != NULL) currentContext->espnow->recv_cb = cb; 
	return ESP_OK; 
}

// VERY very limited context to support two modules running under minimal
// premise that they are separate ESP32s.  Only provides a unique MAC,
// and unique callback vectors for a few callbacks.


Csim_Module::Csim_Module() { 
	assert(currentContext == &defaultContext); // someone didn't reset currentContext after module constructor
	sim().modules.push_back(this);
}

int main(int argc, char **argv) {
	sim().main(argc, argv);
}

void Csim::parseArgs(int argc, char **argv) {
	for(char **a = argv; a < argv+argc; a++) {
		if (strcmp(*a, "--serial") == 0) {
			printf("--serial is depricated, use --serialConsole\n");
			::exit(-1);
		}
		else if (strcmp(*a, "--serialConsole") == 0) sscanf(*(++a), "%d", &Serial.toConsole); 
		else if (strcmp(*a, "--wifi-errors") == 0) sscanf(*(++a), "%d", &WiFi.simulatedFailMinutes); 
		else if (strcmp(*a, "--seconds") == 0) sscanf(*(++a), "%lf", &seconds); 
		else if (strcmp(*a, "-s") == 0) sscanf(*(++a), "%lf", &seconds); 
		else if (strcmp(*a, "--boot-time") == 0) sscanf(*(++a), "%ld", &bootTimeUsec); 
		else if (strcmp(*a, "--stop-time-usec") == 0) {
			unsigned long long stop;
			sscanf(*(++a), "%llu", &stop);
			stopTimeUsec = stop;
		}
		else if (strcmp(*a, "--show-args") == 0) showArgs = true; 
		else if (strcmp(*a, "--reset-reason") == 0) sscanf(*(++a), "%d", &resetReason); 
		else if (strcmp(*a, "--mac") == 0) { 
			sscanf(*(++a), "%lx", &defaultContext.mac);
		} else if (strcmp(*a, "--espnowPipe") == 0) { 
			defaultContext.espnow = new ESPNOW_csimPipe(*(++a), *(++a));
		} else if (strcmp(*a, "--espnowOneProg") == 0) { 
			defaultContext.espnow = new ESPNOW_csimOneProg();
		} else if (strcmp(*a, "--interruptFile") == 0) { 
			intMan.setInterruptFile(*(++a));
		} else if (strcmp(*a, "--button") == 0) {
					int pin, clicks = 1, longclick = 0;
					float tim;
					sscanf(*(++a), "%f,%d,%d,%d", &tim, &pin, &clicks, &longclick);
					Csim_pins().addPress(pin, tim, clicks, longclick);
		} else if (strcmp(*a, "--serialInput") == 0) {
			float seconds;
			sscanf(*(++a), "%f", &seconds);
			Serial.scheduleInput(seconds * 1000, String(*(++a)) + "\n");
		
		} else if (strcmp(*a, "--serial2Input") == 0) {
			float seconds;
			sscanf(*(++a), "%f", &seconds);
			Serial2.scheduleInput(seconds * 1000, String(*(++a)) + "\n");
		} else for(int i = 0; i < modules.size(); i++) { // avoid iterator, modules may be added during setup() calls
			modules[i]->parseArg(a, argv + argc);
		}
	}
}

// TODO IDEA: simulate RTC memory with a special linkage section that 
// is saved and restored to disk 
#define RTC_NOINIT_ATTR __attribute__((section("CSIM_RTC_MEM")))
static RTC_NOINIT_ATTR int rtc_dummy;

extern uint8_t __start_CSIM_RTC_MEM, __stop_CSIM_RTC_MEM;

static void csim_save_rtc_mem() {
	uintptr_t start = reinterpret_cast<uintptr_t>(&__start_CSIM_RTC_MEM);
	uintptr_t stop = reinterpret_cast<uintptr_t>(&__stop_CSIM_RTC_MEM);
	assert(stop >= start);
	size_t len = stop - start;
	int fd = open("./csim_rtc.bin", O_CREAT | O_WRONLY, 0644);
	write(fd, reinterpret_cast<const void *>(start), len);
	close(fd);
}

static void csim_load_rtc_mem(int resetReason) {
	uintptr_t start = reinterpret_cast<uintptr_t>(&__start_CSIM_RTC_MEM);
	uintptr_t stop = reinterpret_cast<uintptr_t>(&__stop_CSIM_RTC_MEM);
	assert(stop >= start);
	size_t len = stop - start;
	// The linker symbols do not describe a C++ object to __builtin_object_size,
	// so fortified memset() rejects this valid linker-section range under -O2.
	// Byte stores express the intended operation without that false positive.
	for (size_t i = 0; i < len; ++i)
		reinterpret_cast<volatile uint8_t *>(start)[i] = 0xde;
	if (resetReason == 5) {
		int fd = open("./csim_rtc.bin", O_RDONLY, 0644);
		vector<uint8_t> saved(len);
		read(fd, saved.data(), len);
		for (size_t i = 0; i < len; ++i)
			reinterpret_cast<volatile uint8_t *>(start)[i] = saved[i];
		close(fd);
	}
}

void Csim::main(int argc, char **argv) {
	assert(currentContext == &defaultContext); // someone didn't reset currentContext after module constructor
	this->argc = argc;
	this->argv = argv;
	Serial.toConsole = true;
	parseArgs(argc, argv); // skip argv[0]
	if (stopTimeUsec == 0 && seconds >= 0)
		stopTimeUsec = bootTimeUsec + (uint64_t)(seconds * 1000000.0);
	// A normal invocation is a fresh boot.  The context checkpoint is consumed
	// only by the deep-sleep exec path, which supplies reset reason 5.
	if (resetReason == 5)
		loadContextSleepState();
	if (showArgs) { 
		printf("args: ");
			for(char **a = argv; a < argv+argc; a++) 
				printf("%s ", *a);
		printf("\n");
	}
	csim_load_rtc_mem(resetReason);
	printf("rtc_dummy: %d\n", rtc_dummy);
	rtc_dummy = rtc_dummy + 1;

	for(int i = 0; i < modules.size(); i++)  {// avoid iterator, modules may be added during setup() calls
		if (modules[i]->context != NULL && modules[i]->context->sleeping)
			continue;
		// A process-wide deep-sleep re-exec can have been caused by a
		// different private context.  An awake context may therefore see
		// setup() again without having entered its own deep sleep; context
		// firmware must restore its state idempotently.
		CsimScopedContextSwap swap(modules[i]->context);		
		modules[i]->setup();
	}
	assert(currentContext == &defaultContext); // someone didn't reset currentContext after module setup
	setup();

	uint64_t lastMillis = 0;
	while(stopTimeUsec == 0 || bootTimeUsec + _micros < stopTimeUsec) {
		lastRealTimeMsec = realTimeMsec;
		struct timeval tv;
		gettimeofday(&tv,NULL);
		realTimeMsec = tv.tv_sec * 1000 +  tv.tv_usec / 1000;

		uint64_t now = millis();
		wakeDueContexts();
		//for(vector<Csim_Module *>::iterator it = modules.begin(); it != modules.end(); it++) 
		for(auto it : modules) {
			if (it->context != NULL && it->context->sleeping)
				continue;
			CsimScopedContextSwap swap(it->context);		
			it->loopActive = true;
			it->loop();
			it->loopActive = false;
		}
		assert(currentContext == &defaultContext); // someone didn't reset currentContext after module loop
		loop();
		intMan.run();

		//if (floor(now / 1000) != floor(lastMillis / 1000)) { 
		//	Csim_JDisplay_forceUpdate();	
		//}
		lastMillis = now;
	}
}
void Csim::exit() { 
	for (auto module : modules) {
		CsimScopedContextSwap swap(module->context);
		module->done();
	}
	::exit(0);
}
void Csim::delayMicroseconds(long long us) { 
	do {
		int step = min(100000LL, us);
		_micros += step;
		wakeDueContexts();
		for(auto it : modules) {
			if (it->loopActive == false
					&& (it->context == NULL || !it->context->sleeping)) {
				CsimScopedContextSwap swap(it->context);		
				it->loopActive = true;
				it->loop();
				it->loopActive = false;
			}
		}
		intMan.run();
		us -= step;
		if (sim().stopTimeUsec != 0 && sim().bootTimeUsec + micros() > sim().stopTimeUsec)
			Csim_exit();
	} while(us > 0);
}

void delayMicroseconds(int m) { sim().delayMicroseconds(m); }

uint32_t micros() { return _microsMax > 0 ? ++_micros & _microsMax : ++_micros; }
uint32_t millis() { return ++_micros / 1000; }
int rtc_get_reset_reason(int) { return sim().resetReason; } 

int FakeWiFi::status() {
   bool disable = simulatedFailMinutes > 0 && 
	   ((micros() + sim().bootTimeUsec) / 1000000 / 60) 
	   % simulatedFailMinutes == simulatedFailMinutes - 1; 
   if (disable) curStatus = WL_DISCONNECTED; 
   return curStatus; 
} 

void FakeSerial::printf(const char  *f, ...) { 
	va_list args;
	va_start (args, f);
	if (toConsole) 
		vprintf(f, args);
	va_end (args);
}

int FakeSerial::available() {
	if (inputLine.length() > 0)
		return inputLine.length(); 
	if (inputQueue.size() > 0 && millis() >= inputQueue[0].first) 
		return inputQueue[0].second.length();
	return 0;
}
int FakeSerial::readBytes(uint8_t *b, int l) {
	if (inputLine.length() == 0 && inputQueue.size() > 0 && millis() >= inputQueue[0].first) { 
		inputLine = inputQueue[0].second;
		inputQueue.pop_front();
	}
	int rval =  min(inputLine.length(), l);
	if (rval > 0) {
		strncpy((char *)b, inputLine.c_str(), rval);
		if (rval < inputLine.length())
			inputLine = inputLine.substring(rval, -1);
		else 
			inputLine = "";
	}
	return rval;
} 

int FakeSerial::read() { 
	uint8_t b;
	if (readBytes(&b, 1) == 1)
		return b;
	return -1; 
}

void FakeCAN::run() { 
	if (callback == NULL || !simFile) 
		return;
	
	if (simFileLine.size() == 0) { 
		std::getline(simFile, simFileLine);
		if (sscanf(simFileLine.c_str(), " (%f) can0 %x [%d]", 
			&pendingPacketTs, &packetAddr, &packetLen) != 3) 
			return;
	}

	if (simFileLine.size() > 0) { 
		packetByteIndex = 0;
		if (firstPacketTs == 0) firstPacketTs = pendingPacketTs;
		if (micros() > (pendingPacketTs - firstPacketTs) * 1000000.0) {
			int remain = packetLen;
			while(remain > 0) { 
				int l = min(remain, 8);
				callback(l);
				remain -= 8;
			} 
			simFileLine = ""; 
		}
	}
}  

int FakeCAN::read() { 
	char *p = strchr((char *)simFileLine.c_str(), ']');
	if (p == NULL) return 0;
	for (int i = 0; i < packetByteIndex / 8 + 1; i++) {
		p = strchr(p + 1, ' ');
		if (p == NULL) return 0;
	}
	unsigned int rval;
	char b[4];
	b[0] = *(p + 1 + (packetByteIndex % 8) * 2);
	b[1] = *(p + 2 + (packetByteIndex % 8) * 2);
	b[3] = 0;
	if (sscanf(b, "%02x", &rval) != 1)
		return 0;
	packetByteIndex++;
	return rval; 
}

fs::File::File(const char *fn, int m) {
	mkdir("./SD", 0755);
	filename = string("./SD/") + fn;
	fflush(stdout);
	int mode = 0;
	if (m & F_CREAT) mode |= O_CREAT;
	if (m & F_APPEND) mode |= O_APPEND;
	if (m & F_TRUNC) mode |= O_TRUNC;
	if (m & F_WRITE) mode |= O_WRONLY;
	if (m & F_RDONLY) mode |= O_RDONLY;
	fd = open(filename.c_str(), mode, 0644);
}

string FakeSPIFFS::root() const {
	char mac[32];
	snprintf(mac, sizeof(mac), "%012llx",
			(unsigned long long)(currentContext->mac & 0xffffffffffffULL));
	mkdir("./csim-fs", 0755);
	string result = string("./csim-fs/") + mac;
	mkdir(result.c_str(), 0755);
	return result;
}

string FakeSPIFFS::path(const char *name) const {
	if (name == NULL || strstr(name, "..") != NULL)
		return string();
	const char *relative = name[0] == '/' ? name + 1 : name;
	return root() + "/" + relative;
}

fs::File::File(const char *fn, const char *m) {
	// Each CSIM context models an independent device flash chip.  Firmware can
	// therefore use its normal absolute paths without context-ID conventions.
	filename = LittleFS.path(fn);
	if (filename.empty())
		return;
	int mode = O_RDONLY;
	if (strcmp(m, "a") == 0) mode = O_CREAT | O_APPEND | O_WRONLY;
	if (strcmp(m, "r") == 0) mode = O_CREAT | O_RDONLY;
	if (strcmp(m, "w") == 0) mode = O_CREAT | O_TRUNC | O_WRONLY;
	if (strcmp(m, "r+") == 0) mode = O_CREAT | O_RDWR;
	if (strcmp(m, "w+") == 0) mode = O_CREAT | O_TRUNC | O_RDWR;
	fd = open(filename.c_str(), mode, 0644);
}


#endif
