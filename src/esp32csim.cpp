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

uint64_t csim_mac = 0xffeeddaabbcc;
void esp_read_mac(uint8_t *out, int) {
	uint64_t nmac = htonll(csim_mac);
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

Csim_flags csim_flags;

void Csim_exit() { 	sim().exit(); }

uint64_t sleep_timer = 0;
vector<deepSleepHookT> deepSleepHooks;
void csim_onDeepSleep(deepSleepHookT func) { deepSleepHooks.push_back(func); }
void esp_deep_sleep_start() {
	double newRunSec = -1;
	if (sim().seconds >= 0) {
		newRunSec = sim().seconds - (sleep_timer + _micros) / 1000000;
		if (newRunSec < 0) { 
			fflush(stdout);
			Csim_exit();
		}
	}

	for (auto i : deepSleepHooks) i(sleep_timer);
	csim_save_rtc_mem(); 

	char *argv[128];
	int argc = 0; 
	// strip out all --boot-time, --seconds, --reset-reason and --show-args command line arguments
	for(char *const *p = sim().argv; *p != NULL; p++) {
		if (strcmp(*p, "--boot-time") == 0) p++;
		else if (strcmp(*p, "--seconds") == 0) p++;
		else if (strcmp(*p, "-s") == 0) p++;
		else if (strcmp(*p, "--reset-reason") == 0) p++;
		else if (strcmp(*p, "--show-args") == 0) {/*skip arg*/}
		else argv[argc++] = *p;
	}
	char bootTimeBuf[32], secondsBuf[32];
	snprintf(bootTimeBuf, sizeof(bootTimeBuf), "%ld", sim().bootTimeUsec + sleep_timer + _micros);
	argv[argc++] = (char *)"--boot-time";
	argv[argc++] = bootTimeBuf; 
	snprintf(secondsBuf, sizeof(secondsBuf), "%f", newRunSec);
	argv[argc++] = (char *)"--seconds";
	argv[argc++] = secondsBuf; 
	argv[argc++] = (char *)"--reset-reason";
	argv[argc++] = (char *)"5";
	argv[argc++] = (char *)"--show-args";
	argv[argc++] = NULL;
	fflush(stdout);
	execv("./csim", argv); 
}
void esp_light_sleep_start() {
	delayMicroseconds(0); // run csim hooks 
	_micros += sleep_timer; 
	if (sim().seconds > 0 && micros() / 1000000.0 > sim().seconds)
		Csim_exit();
} 

ESPNOW_csimInterface *ESPNOW_sendHandler = NULL;
esp_now_send_cb_t ESP32_esp_now_send_cb = NULL;
esp_now_recv_cb_t ESP32_esp_now_recv_cb = NULL;

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

void ESPNOW_csimOneProg::send(const uint8_t *mac_addr, const uint8_t *data, int data_len) {//override {
	SimPacket p; 
	memcpy(p.mac, mac_addr, sizeof(p.mac));
	const char *cp = (const char *)data;
	p.data = string(cp, cp + data_len);
	pktQueue.push_back(p);
}
void ESPNOW_csimOneProg::loop() { // override
	for(auto pkt : pktQueue) {
		if (ESP32_esp_now_recv_cb != NULL) {
			if (SIMFAILURE("espnow-rx") || SIMFAILURE("espnow"))
				continue;
			ESP32_esp_now_recv_cb(pkt.mac, (const uint8_t *)pkt.data.c_str(), pkt.data.length());
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
	char buf[512];
	int n;
	while(fdIn > 0 && (n = ::read(fdIn, buf, sizeof(buf))) > 0) { 
		if (SIMFAILURE("espnow-rx") || SIMFAILURE("espnow"))
		return;

		if (ESP32_esp_now_recv_cb != NULL) {
			ESP32_esp_now_recv_cb(mac, (uint8_t *)buf, n);
		}	
	}
} 

int esp_now_send(const uint8_t*mac, const uint8_t*data, size_t len) {
	if (SIMFAILURE("espnow-tx") || SIMFAILURE("espnow"))
		return ESP_OK;

	if (ESP32_esp_now_send_cb != NULL)
		ESP32_esp_now_send_cb(mac, ESP_NOW_SEND_SUCCESS); 
	if (ESPNOW_sendHandler != NULL) 
		ESPNOW_sendHandler->send(mac, data, len); 
	return ESP_OK; 
}


Csim_Module::Csim_Module() { 
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
		else if (strcmp(*a, "--show-args") == 0) showArgs = true; 
		else if (strcmp(*a, "--reset-reason") == 0) sscanf(*(++a), "%d", &resetReason); 
		else if (strcmp(*a, "--mac") == 0) { 
			sscanf(*(++a), "%lx", &csim_mac);
		} else if (strcmp(*a, "--espnowPipe") == 0) { 
			ESPNOW_sendHandler= new ESPNOW_csimPipe(*(++a), *(++a));
		} else if (strcmp(*a, "--espnowOneProg") == 0) { 
			ESPNOW_sendHandler= new ESPNOW_csimOneProg();
			csim_flags.OneProg = true;
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
		} else for(vector<Csim_Module *>::iterator it = modules.begin(); it != modules.end(); it++) {
			(*it)->parseArg(a, argv + argc);
		}
	}
}

// TODO IDEA: simulate RTC memory with a special linkage section that 
// is saved and restored to disk 
#define RTC_NOINIT_ATTR __attribute__((section("CSIM_RTC_MEM")))
static RTC_NOINIT_ATTR int rtc_dummy;

extern uint8_t __start_CSIM_RTC_MEM, __stop_CSIM_RTC_MEM;

static void csim_save_rtc_mem() {
	size_t len = &__stop_CSIM_RTC_MEM - &__start_CSIM_RTC_MEM;
	int fd = open("./csim_rtc.bin", O_CREAT | O_WRONLY, 0644);
	write(fd, &__start_CSIM_RTC_MEM, len);
	close(fd);
}

static void csim_load_rtc_mem(int resetReason) {
	size_t len = &__stop_CSIM_RTC_MEM - &__start_CSIM_RTC_MEM;
	memset(&__start_CSIM_RTC_MEM, 0xde, len);
	if (resetReason == 5) {  
		int fd = open("./csim_rtc.bin", O_RDONLY, 0644);
		read(fd, &__start_CSIM_RTC_MEM, len);
		close(fd);
	}
}

void Csim::main(int argc, char **argv) {
	this->argc = argc;
	this->argv = argv;
	Serial.toConsole = true;
	parseArgs(argc, argv); // skip argv[0]
	if (showArgs) { 
		printf("args: ");
			for(char **a = argv; a < argv+argc; a++) 
				printf("%s ", *a);
		printf("\n");
	}
	csim_load_rtc_mem(resetReason);
	printf("rtc_dummy: %d\n", rtc_dummy);
	rtc_dummy = rtc_dummy + 1;

	for(int i = 0; i < modules.size(); i++) // avoid iterator, modules may be added during setup() calls
		modules[i]->setup();
	setup();

	uint64_t lastMillis = 0;
	while(seconds <= 0 || _micros / 1000000.0 < seconds) {
		uint64_t now = millis();
		//for(vector<Csim_Module *>::iterator it = modules.begin(); it != modules.end(); it++) 
		for(auto it : modules) {
			it->loop();
		}
		loop();
		intMan.run();

		//if (floor(now / 1000) != floor(lastMillis / 1000)) { 
		//	Csim_JDisplay_forceUpdate();	
		//}
		lastMillis = now;
	}
}
void Csim::exit() { 
	for(vector<Csim_Module *>::iterator it = modules.begin(); it != modules.end(); it++) 
		(*it)->done();	
	::exit(0);
}
void Csim::delayMicroseconds(long long us) { 
	do {
		int step = min(100000LL, us);
		_micros += step;
		for(auto it : modules) {
			it->loop();
		}
		intMan.run();
		us -= step;
		if (sim().seconds > 0 && micros() / 1000000.0 > sim().seconds)
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

fs::File::File(const char *fn, const char *m) {
	mkdir("./spiff", 0755);
	filename = string("./spiff/") + fn;
	int mode = O_RDONLY;
	if (strcmp(m, "a") == 0) mode = O_CREAT | O_APPEND | O_WRONLY;
	if (strcmp(m, "r") == 0) mode = O_CREAT | O_RDONLY;
	if (strcmp(m, "w") == 0) mode = O_CREAT | O_TRUNC | O_WRONLY;
	if (strcmp(m, "r+") == 0) mode = O_CREAT | O_RDWR;
	if (strcmp(m, "w+") == 0) mode = O_CREAT | O_TRUNC | O_RDWR;
	fd = open(filename.c_str(), mode, 0644);
}


#endif
