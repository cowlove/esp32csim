#include "esp32csim.h"
#include "driver/twai.h"


class CsimTwai : public Csim_Module {
    ifstream ifile;
    uint32_t firstTimestamp;
    void parseArg(char **&a, char **) override {
        if (strcmp(*a, "--can") == 0) {
            ifile = ifstream(*(++a), ios_base::in);
        }
    }
    twai_message_t nextPacket;
    uint32_t nextPacketMicros = 0;
    bool nextPacketValid = false;
    void loadNextPacket() { 
        string s;
        twai_message_t &p = nextPacket;
        std::getline(ifile, s);
        int len, d[8] = {0};
        int n = sscanf(s.c_str(), "CAN %x %x %x %d %x %x %x %x %x %x %x %x",
            &nextPacketMicros, &p.flags, &p.identifier, &len,
            &d[0], &d[1], &d[2], &d[3],
            &d[4], &d[5], &d[6], &d[7]);
        p.data_length_code = len; 
        for (int i = 0; i < 8; i++) p.data[i] = d[i];
        nextPacketValid = (n >=4);
        if (firstTimestamp == 0) firstTimestamp = nextPacketMicros;
    }
public:
    int twai_receive(twai_message_t *p, int tmo) {
        if (!nextPacketValid) loadNextPacket();
        if (!nextPacketValid) return 1;
        if (micros() < nextPacketMicros - firstTimestamp) return 1;
        *p = nextPacket;
        nextPacketValid = false;
        return 0;
    } 
} twai;

int twai_driver_install(twai_general_config_t *, twai_timing_config_t *, twai_filter_config_t *) {
    return ESP_OK;
}
int twai_start() { return ESP_OK; }
int twai_driver_uninstall() { return ESP_OK; }
int twai_stop() { return ESP_OK; }

int twai_receive(twai_message_t *p, int tmo) { return twai.twai_receive(p, tmo); }
int twai_transmit(twai_message_t *, int) { return 1; }

int twai_get_status_info(twai_status_info_t *) { return ESP_OK; }
