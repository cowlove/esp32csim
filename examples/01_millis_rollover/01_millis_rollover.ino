#ifdef CSIM
#include "esp32csim.h"
#endif 

void setup() { 
	Serial.begin(115200);
}

uint32_t nextPrintMs = 0; 
int hours = 0;
void loop() { 
	if (millis() > nextPrintMs) {
		Serial.printf("%d hours passed, millis() == %08x\n", hours++, (unsigned int)millis());
		nextPrintMs = millis() + 3600 * 1000;
	}
	delay(1600 * 1000);
}

