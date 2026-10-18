// Local configuration template.
// Copy this file to main/local_config.h (gitignored) and set your own values.

// RF remote ID your HRU300 is paired with. Sniff it from a physical remote
// (e.g. with the ithowifi firmware's RF log) or pick a fresh ID and join it
// to the unit (see README). Format: three bytes, e.g. 0x12, 0x34, 0x56
#define ITHO_REMOTE_ID 0xDE, 0xAD, 0x01

// Optional: RF ID of the ventilation unit itself. When set, its 31DA status
// broadcasts are used as extra fan feedback; when unset, 31DA is ignored and
// each one is logged with its sender ID ("RF 31DA from xx:xx:xx ignored").
// #define ITHO_UNIT_ID 0x12, 0x34, 0x56
