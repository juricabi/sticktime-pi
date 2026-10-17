/* VTX tables like Betaflight's: bands (name, letter, factory flag, up to 8 frequencies) and power
 * levels (the value sent to the VTX and a label). The value's unit depends on the protocol:
 * SmartAudio 1.0 the PA's DAC value, SmartAudio 2.0 a level 0..3, SmartAudio 2.1 dBm, Tramp mW.
 * A factory band is one of the VTX's own (SmartAudio sets it by channel number: band position in
 * the table * channels + channel); other bands are set by frequency.
 *
 * Files are the Betaflight Configurator's VTX table JSON, the format VTX makers publish:
 *   { "description": "...", "version": "1.0",
 *     "vtx_table": { "bands_list": [ { "name": "BOSCAM_A", "letter": "A", "is_factory_band": true,
 *                                      "frequencies": [5865, 5845, ...] }, ... ],
 *                    "powerlevels_list": [ { "value": 0, "label": "25" }, ... ] } } */
#ifndef VTX_TABLE_H
#define VTX_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VTX_MAX_BANDS 8
#define VTX_MAX_CHANNELS 8
#define VTX_MAX_POWERS 8
#define VTX_TABLE_FILE_MAX (64 * 1024)
#define VTX_FREQ_MIN 1000          /* a table's frequencies: 0 (no such channel) or 1000..9999 MHz */
#define VTX_FREQ_MAX 9999

typedef struct {
  char name[16];                   /* Betaflight keeps 8 characters */
  char letter;
  bool factory;
  uint16_t freq[VTX_MAX_CHANNELS]; /* MHz; 0 = no such channel */
} vtx_band_t;

typedef struct {
  uint16_t value;
  char label[8];                   /* Betaflight shows 3 characters */
} vtx_power_t;

typedef struct {
  char description[128];
  int nbands, nchannels, npowers;
  vtx_band_t band[VTX_MAX_BANDS];
  vtx_power_t power[VTX_MAX_POWERS];
} vtx_table_t;

typedef enum {
  VTX_TABLE_SA1,                   /* SmartAudio 1.0: DAC values 7, 16, 25, 40 */
  VTX_TABLE_SA2,                   /* SmartAudio 2.0: levels 0..3 */
  VTX_TABLE_SA21,                  /* SmartAudio 2.1: the dBm levels the VTX reports */
  VTX_TABLE_TRAMP,                 /* Tramp: 25, 100, 200, 400, 600 mW, bands by frequency */
} vtx_table_kind_t;

/* The built-in table: bands A, B, E, F, R (factory for SmartAudio: the VTX's 40 channels), power
 * levels by kind. dbm/ndbm: SA2.1's reported levels (NULL/0: 14, 20, 26 dBm). */
void vtx_table_default(vtx_table_t *t, vtx_table_kind_t kind, const uint8_t *dbm, int ndbm);

/* Parse a Betaflight Configurator VTX table. 0 ok; -1 and a message for people in err. */
int vtx_table_parse_json(const char *json, size_t len, vtx_table_t *t, char *err, size_t errlen);
/* The table as Betaflight Configurator JSON (malloc'd, NUL-terminated; NULL when out of memory). */
char *vtx_table_to_json(const vtx_table_t *t, size_t *len);

int  vtx_table_band(const vtx_table_t *t, char letter);          /* index (case-insensitive), -1 */
uint16_t vtx_table_freq(const vtx_table_t *t, int band, int channel);   /* channel 1-based; 0 if none */
/* the band/channel of a frequency (the first match; factory bands first), false when not there */
bool vtx_table_find_freq(const vtx_table_t *t, uint16_t freq, int *band, int *channel);
int  vtx_table_power_level(const vtx_table_t *t, int value);     /* 1-based level of a value, 0 */

#endif
