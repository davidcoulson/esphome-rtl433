// Per-band decoder sets: when rtl_433 hops, only the decoders chosen for the new frequency stay registered.
// Configured by the ESPHome component before rtl_433 starts; applied from rtl_433's main task (the one
// that runs the decoders), at start-up and on every hop.
#include <stdlib.h>
#include <string.h>

#include "rtl_433.h"
#include "r_api.h"
#include "r_device.h"
#include "r_private.h"
#include "rtl_433_devices.h"
#include "logger.h"

#include "rtl433_port.h"

// Decoder symbol names in the order rtl_433 numbers them (r_init() builds cfg->devices the same way)
#define DECL(name) #name,
static char const *const decoder_names[] = {DEVICES};
#undef DECL
#define NUM_DECODER_NAMES (sizeof(decoder_names) / sizeof(decoder_names[0]))

static char *band_decoders[MAX_FREQS];  // comma-separated names or numbers; NULL = rtl_433 defaults
static int bands_configured = 0;
static int applied_band = -1;
static int band_count = 0;  // the configured hop list's length, captured before any manual retune

void rtl433_port_set_band_decoders(int band, char const *list) {
  if (band < 0 || band >= MAX_FREQS)
    return;
  free(band_decoders[band]);
  band_decoders[band] = (list && *list) ? strdup(list) : NULL;
  if (band_decoders[band])
    bands_configured = 1;
}

static r_device *find_decoder(r_cfg_t *cfg, char const *token) {
  char *end;
  long num = strtol(token, &end, 10);
  if (*token && *end == '\0') {  // a protocol number, as for -R
    return (num >= 1 && num <= cfg->num_r_devices) ? &cfg->devices[num - 1] : NULL;
  }
  for (unsigned i = 0; i < NUM_DECODER_NAMES && (int) i < cfg->num_r_devices; i++) {
    if (!strcmp(decoder_names[i], token))
      return &cfg->devices[i];
  }
  return NULL;
}

// Only the built-in decoders take part in band switching; a flex decoder (-X) or anything else the
// user registered by hand keeps running on every band
static int is_builtin(r_cfg_t *cfg, r_device *dev) {
  for (int i = 0; i < cfg->num_r_devices; i++)
    if (!strcmp(cfg->devices[i].name, dev->name))
      return 1;
  return 0;
}

static void unregister_all(r_cfg_t *cfg) {
  list_t *devs = &cfg->demod->r_devs;
  for (size_t i = devs->len; i-- > 0;) {
    r_device *dev = devs->elems[i];
    if (dev != NULL && is_builtin(cfg, dev))
      unregister_protocol(cfg, dev);
  }
}

static int register_list(r_cfg_t *cfg, char const *list) {
  char *copy = strdup(list);
  int count = 0;
  for (char *tok = strtok(copy, ","); tok != NULL; tok = strtok(NULL, ",")) {
    r_device *dev = find_decoder(cfg, tok);
    if (dev == NULL) {
      print_logf(LOG_WARNING, "Decoders", "Unknown decoder \"%s\"", tok);
      continue;
    }
    unregister_protocol(cfg, dev);  // no duplicates when bands share a decoder
    register_protocol(cfg, dev, NULL);
    count++;
  }
  free(copy);
  return count;
}

// A frequency set from Home Assistant (/cmd center_frequency) ends hopping, and it may not be any of the
// configured bands: run every band's decoders (or the defaults, if some band uses the defaults).
void rtl433_port_manual_tune(r_cfg_t *cfg) {
  if (!bands_configured)
    return;
  applied_band = -2;
  unregister_all(cfg);
  for (int i = 0; i < MAX_FREQS; i++) {
    if (band_decoders[i] == NULL && i < band_count) {
      unregister_all(cfg);
      register_all_protocols(cfg, 0);
      print_log(LOG_NOTICE, "Decoders", "Manual frequency: rtl_433 default decoders");
      return;
    }
    if (band_decoders[i] != NULL)
      register_list(cfg, band_decoders[i]);
  }
  print_log(LOG_NOTICE, "Decoders", "Manual frequency: every band's decoders");
}

void rtl433_port_apply_band(r_cfg_t *cfg) {
  if (!bands_configured || applied_band == -2)  // -2: manually tuned, hopping is over
    return;
  if (band_count == 0)
    band_count = cfg->frequencies;
  int band = cfg->frequency_index;
  if (band == applied_band)
    return;
  applied_band = band;

  unregister_all(cfg);

  char const *list = band < MAX_FREQS ? band_decoders[band] : NULL;
  if (list == NULL) {
    register_all_protocols(cfg, 0);
    print_logf(LOG_NOTICE, "Decoders", "Band %d: rtl_433 default decoders", band);
    return;
  }
  int count = register_list(cfg, list);
  print_logf(LOG_NOTICE, "Decoders", "Band %d: %d decoder(s): %s", band, count, list);
}
