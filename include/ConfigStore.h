
#define CONFIG_FLAG_VALID 0x01
#define CONFIG_FLAG_STATIC_IP 0x02

#define BLYNK_PROV_ERR_NONE 0  // All good
#define BLYNK_PROV_ERR_CONFIG \
    700  // Invalid config from app (malformed token,etc)
#define BLYNK_PROV_ERR_NETWORK 701   // Could not connect to the router
#define BLYNK_PROV_ERR_CLOUD 702     // Could not connect to the cloud
#define BLYNK_PROV_ERR_TOKEN 703     // Invalid token error (after connection)
#define BLYNK_PROV_ERR_INTERNAL 704  // Other issues (i.e. hardware failure)

struct ConfigStore {
    uint32_t magic;
    char version[15];
    uint8_t flags;

    char wifiSSID[34];
    char wifiPass[64];

    char cloudToken[34];
    char cloudHost[34];
    uint16_t cloudPort;

    uint32_t staticIP;
    uint32_t staticMask;
    uint32_t staticGW;
    uint32_t staticDNS;
    uint32_t staticDNS2;

    int last_error;

    void setFwVer(const char* ver) {
        memset(version, 0, sizeof(version));
        strncpy(version, ver, sizeof(version) - 1);
    }

    void setFlag(uint8_t mask, bool value) {
        if (value) {
            flags |= mask;
        } else {
            flags &= ~mask;
        }
    }

    bool getFlag(uint8_t mask) { return (flags & mask) == mask; }
} __attribute__((packed));

ConfigStore configStore;

const ConfigStore configDefault = {0x626C6E6B,
                                   BLYNK_FIRMWARE_VERSION,
                                   0x00,

                                   "",
                                   "",

                                   "invalid token",
                                   CONFIG_DEFAULT_SERVER,
                                   CONFIG_DEFAULT_PORT,
                                   0,
                                   BLYNK_PROV_ERR_NONE};

template <typename T, int size>
void CopyString(const String& s, T (&arr)[size]) {
    s.toCharArray(arr, size);
}

static bool config_load_blnkopt() {
    static const char blnkopt[] = "blnkopt\0" BLYNK_PARAM_KV(
        "ssid", BLYNK_PARAM_PLACEHOLDER_64 BLYNK_PARAM_PLACEHOLDER_64
                    BLYNK_PARAM_PLACEHOLDER_64 BLYNK_PARAM_PLACEHOLDER_64)
        BLYNK_PARAM_KV("host", CONFIG_DEFAULT_SERVER)
            BLYNK_PARAM_KV("port", BLYNK_TOSTRING(CONFIG_DEFAULT_PORT)) "\0";

    BlynkParam prov(blnkopt + 8, sizeof(blnkopt) - 8 - 2);
    BlynkParam::iterator ssid = prov["ssid"];
    BlynkParam::iterator pass = prov["pass"];
    BlynkParam::iterator auth = prov["auth"];
    BlynkParam::iterator host = prov["host"];
    BlynkParam::iterator port = prov["port"];

    if (!(ssid.isValid() && auth.isValid())) {
        return false;
    }

    // reset to defaut before loading values from blnkopt
    configStore = configDefault;

    if (ssid.isValid()) {
        CopyString(ssid.asStr(), configStore.wifiSSID);
    }
    if (pass.isValid()) {
        CopyString(pass.asStr(), configStore.wifiPass);
    }
    if (auth.isValid()) {
        CopyString(auth.asStr(), configStore.cloudToken);
    }
    if (host.isValid()) {
        CopyString(host.asStr(), configStore.cloudHost);
    }
    if (port.isValid()) {
        configStore.cloudPort = port.asInt();
    }

    return true;
}

#include <Preferences.h>

void config_load() {
    Preferences prefs;
    if (prefs.begin("blynk", true)) {  // read-only
        memset(&configStore, 0, sizeof(configStore));
        prefs.getBytes("config", &configStore, sizeof(configStore));
        if (configStore.magic != configDefault.magic) {
            DEBUG_PRINT("Using default config.");
            configStore = configDefault;
        }
    } else {
        DEBUG_PRINT("Config read failed");
    }
}

bool config_save() {
    Preferences prefs;
    if (prefs.begin("blynk", false)) {  // writeable
        prefs.putBytes("config", &configStore, sizeof(configStore));
        DEBUG_PRINT("Configuration stored to flash");
        return true;
    } else {
        DEBUG_PRINT("Config write failed");
        return false;
    }
}

// bool pin_reset() {
//     Preferences prefs;
//     if (prefs.begin("smartlock", false)) {
//         prefs.clear();
//         DEBUG_PRINT("Pin reset to default pin");
//         prefs.end();
//         return true;
//     } else {
//         DEBUG_PRINT("Config write failed");
//         return false;
//     }
// }

bool flag_reset() {
    Preferences prefs;
    if (prefs.begin("smartlock", false)) {
        prefs.putBool("flag_reset", true);
        DEBUG_PRINT("The door's database is going to reset!");
        prefs.end();
        return true;
    } else {
        DEBUG_PRINT("Config write failed");
        return false;
    }
}

bool config_init() {
    config_load();
    return true;
}

void enterResetConfig() {
    DEBUG_PRINT("Resetting configuration!");
    configStore = configDefault;
    config_save();
    flag_reset();
    BlynkState::set(MODE_WAIT_CONFIG);
}

void config_set_last_error(int error) {
    // Only set error if not provisioned
    if (!configStore.getFlag(CONFIG_FLAG_VALID)) {
        configStore = configDefault;
        configStore.last_error = error;
        BLYNK_LOG2("Last error code: ", error);
        config_save();
    }
}
