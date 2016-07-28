/*
  Copyright 2006-2011 David Robillard <d@drobilla.net>
  Copyright 2006 Steve Harris <steve@plugin.org.uk>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <stdarg.h>
#include <jack/jack.h>
#include <aubio/aubio.h>
#include <aubio/pitch/pitch.h>
#include <aubio/mathutils.h>
#include <Stk.h>
#include <Instrmnt.h>
#include <Voicer.h>
#include <JCRev.h>
#include "utilities.h"
#include <algorithm>

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"
#include "lv2/lv2plug.in/ns/ext/log/logger.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

#define HARMONIZER_URI "http://dsheeler.org/plugins/harmonizer"
#define NUM_STRINGS 3
#define NUM_ONSET_METHODS 9
#define NUM_PITCH_METHODS 6

typedef struct {
  LV2_URID atom_Blank;
  LV2_URID atom_Object;
  LV2_URID atom_Sequence;
  LV2_URID midi_MidiEvent;
  LV2_URID atom_URID;
} MidiGenURIs;

typedef enum {
	HARMONIZER_ONSET_METHOD   = 0,
	HARMONIZER_ONSET_THRESHOLD   = 1,
  HARMONIZER_SILENCE_THRESHOLD = 2,
	HARMONIZER_PITCH_METHOD   = 3,
  HARMONIZER_PITCH_THRESHOLD = 4,
  HARMONIZER_INPUT_1  = 5,
  HARMONIZER_INPUT_2  = 6,
	HARMONIZER_OUTPUT_1 = 7,
  HARMONIZER_OUTPUT_2 = 8,
  HARMONIZER_MIDI_OUT = 9
} PortIndex;

char *onset_methods[NUM_ONSET_METHODS];
char *pitch_methods[NUM_PITCH_METHODS];

using namespace stk;

typedef jack_default_audio_sample_t sample_t;

struct string_info {
  bool         in_use;
  unsigned int inote;
  string_info() : in_use(false), inote(0) {};
};

typedef struct {
  LV2_Atom_Event event;
  uint8_t msg[3];
} MIDI_note_event;


typedef struct {
  aubio_onset_t *onsets[NUM_ONSET_METHODS];
  aubio_onset_t *onsets_2[NUM_ONSET_METHODS];
  aubio_pitch_t *pitches_1[NUM_PITCH_METHODS];
  aubio_pitch_t *pitches_2[NUM_PITCH_METHODS];
  LV2_Log_Log* log;
  LV2_Log_Logger logger;
  LV2_URID_Map* map;
  MidiGenURIs uris;
  LV2_Atom_Forge forge;
  LV2_Atom_Forge_Frame frame;
  uint32_t frame_offset;
  const float* onset_method;
  const float* onset_threshold;
	const float* silence_threshold;
  const float* pitch_method;
  const float* pitch_threshold;
  const float* input_1;
  const float* input_2;
	float*       output_1;
	float*       output_2;
  LV2_Atom_Sequence* midi_out;
  smpl_t bufsize;
  smpl_t hopsize;
  char_t *pitch_unit;
  smpl_t *pitch;
  fvec_t *pitch_1;
  fvec_t *pitch_2;
  uint_t median;
  smpl_t newnote_1;
  smpl_t newnote_2;
  uint_t isready_1;
  uint_t isready_2;
  smpl_t curnote_1;
  smpl_t curnote_2;
  smpl_t curlevel_1;
  smpl_t curlevel_2;
  fvec_t *ab_out_1;
  fvec_t *ab_out_2;
  fvec_t *ab_in[2];
  fvec_t *note_buffer_1;
  fvec_t *note_buffer_2;
  fvec_t *note_buffer2_1;
  fvec_t *note_buffer2_2;
  fvec_t *onset_1;
  fvec_t *onset_2;
  Instrmnt **instrument_1;
  Instrmnt **instrument_2;
  string_info voices[NUM_STRINGS];
  Voicer *voicer_1;
  Voicer *voicer_2;
  StkFloat volume;
  StkFloat t60;
  smpl_t samplerate;
  int nvoices;
  int channels;
  int counter;
  int current_voice;
  int frequency;
  StkFloat feedbackGain;
  StkFloat oldFeedbackGain;
  StkFloat distortionGain;
  StkFloat distortionMix;
  Delay feedbackDelay;
  StkFloat feedbackSample;
} Harmonizer;

const char *err_buf;
int intval;

/** map uris */
static void
map_mem_uris (LV2_URID_Map* map, MidiGenURIs* uris)
{
  uris->atom_Blank         = map->map (map->handle, LV2_ATOM__Blank);
  uris->atom_Object        = map->map (map->handle, LV2_ATOM__Object);
  uris->midi_MidiEvent     = map->map (map->handle, LV2_MIDI__MidiEvent);
  uris->atom_Sequence      = map->map (map->handle, LV2_ATOM__Sequence);
  uris->atom_URID          = map->map (map->handle, LV2_ATOM__URID);
}

/**
 *  * add a midi message to the output port
 *   */
static void
forge_midimessage (Harmonizer* self,
    uint32_t tme,
    const uint8_t* const buffer,
    uint32_t size)
{
  LV2_Atom midiatom;
  midiatom.type = self->uris.midi_MidiEvent;
  midiatom.size = size;

  if (0 == lv2_atom_forge_frame_time (&self->forge, tme)) return;
  if (0 == lv2_atom_forge_raw (&self->forge, &midiatom, sizeof (LV2_Atom))) return;
  if (0 == lv2_atom_forge_raw (&self->forge, buffer, size)) return;
  lv2_atom_forge_pad (&self->forge, sizeof (LV2_Atom) + size);
}

static void
midi_panic (Harmonizer* self)
{
  uint8_t event[3];
  event[2] = 0;

  for (uint32_t c = 0; c < 0xf; ++c) {
    event[0] = 0xb0 | c;
    event[1] = 0x40; // sustain pedal
    forge_midimessage (self, 0, event, 3);
    event[1] = 0x7b; // all notes off
    forge_midimessage (self, 0, event, 3);
#if 0
    event[1] = 0x78; // all sound off
    forge_midimessage (self, 0, event, 3);
#endif
  }
}

void note_append(fvec_t *note_buffer, smpl_t curnote) {
  uint_t i = 0;
  for (i = 0; i < note_buffer->length - 1; i++) {
    note_buffer->data[i] = note_buffer->data[i + 1];
  }
  note_buffer->data[note_buffer->length - 1] = curnote;
  return;
}

uint_t get_note (fvec_t * note_buffer, fvec_t * note_buffer2) {
  uint_t i;
  for (i = 0; i < note_buffer->length; i++) {
    note_buffer2->data[i] = note_buffer->data[i];
  }
  return fvec_median (note_buffer2);
}

void send_noteon_1(smpl_t note, smpl_t level, void *usr) {
  Harmonizer *harm = (Harmonizer *)usr;
  if (note > 0) {
    smpl_t midi_note = floor(0.5 + aubio_freqtomidi(note));
    uint8_t event[3];
    event[0] = 0x90;
    event[1] = (uint8_t)midi_note;
    event[2] = (uint8_t)level;
    forge_midimessage(harm, 0, event, 3);
    harm->voicer_1->noteOn(midi_note, level);
    harm->voicer_1->noteOn(midi_note + 4, level);
    harm->voicer_1->noteOn(midi_note + 7, level);
  }
}

void send_noteoff_1(smpl_t note, smpl_t level, void *usr) {
  Harmonizer *harm = (Harmonizer *)usr;
  smpl_t midi_note = aubio_freqtomidi(note);
  uint8_t event[3];
  event[0] = 0x80;
  event[1] = (uint8_t)midi_note;
  event[2] = (uint8_t)level;
  forge_midimessage(harm, 0, event, 3);
  harm->voicer_1->noteOff(midi_note, level);
  harm->voicer_1->noteOff(midi_note + 4, level);
  harm->voicer_1->noteOff(midi_note + 7, level);
}

void send_noteon_2(smpl_t note, smpl_t level, void *usr) {
  Harmonizer *harm = (Harmonizer *)usr;
  if (note > 0) {
    smpl_t midi_note = aubio_freqtomidi(note);
    harm->voicer_2->noteOn(midi_note, level);
    harm->voicer_2->noteOn(midi_note + 4, level);
    harm->voicer_2->noteOn(midi_note + 7, level);
  }
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
    double                    rate,
    const char*               bundle_path,
    const LV2_Feature* const* features) {
  Harmonizer* harm = (Harmonizer*)malloc(sizeof(Harmonizer));

  for (int i = 0; features[i]; ++i) {
    if (!strcmp (features[i]->URI, LV2_URID__map)) {
      harm->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
      harm->log = (LV2_Log_Log*)features[i]->data;
    }
  }
  lv2_log_logger_init(&harm->logger, harm->map, harm->log);
  if (!harm->map) {
    lv2_log_error (&harm->logger, "MidiGen.lv2 error: Host does not support urid:map\n");
    free (harm);
    return NULL;
  }
  lv2_atom_forge_init (&harm->forge, harm->map);
  map_mem_uris (harm->map, &harm->uris);
  harm->samplerate = (float)rate;
  lv2_log_trace(&harm->logger, "samplerate: %f", harm->samplerate);
  harm->bufsize = 128;
  harm->hopsize = harm->bufsize / 4;
  harm->median = 3;
  harm->onset_1 = new_fvec(1);
  harm->onset_2 = new_fvec(1);
  harm->ab_in[0] = new_fvec(harm->bufsize);
  harm->ab_in[1] = new_fvec(harm->bufsize);
  harm->ab_out_1 = new_fvec(1);
  harm->ab_out_2 = new_fvec(1);
  harm->note_buffer_1 = new_fvec(harm->median);
  harm->note_buffer_2 = new_fvec(harm->median);
  harm->note_buffer2_1 = new_fvec(harm->median);
  harm->note_buffer2_2 = new_fvec(harm->median);
  harm->nvoices = NUM_STRINGS;
  harm->instrument_1 = (Instrmnt **) calloc(harm->nvoices, sizeof(Instrmnt *));
  harm->instrument_2 = (Instrmnt **) calloc(harm->nvoices, sizeof(Instrmnt *));
  harm->voicer_1 = (Voicer *) new Voicer(0.0);
  harm->voicer_2 = (Voicer *) new Voicer(0.0);
  onset_methods[0] = (char*)"default";
  onset_methods[1] = (char*)"energy";
  onset_methods[2] = (char*)"hfc";
  onset_methods[3] = (char*)"complex";
  onset_methods[4] = (char*)"phase";
  onset_methods[5] = (char*)"specdiff";
  onset_methods[6] = (char*)"kl";
  onset_methods[7] = (char*)"mkl";
  onset_methods[8] = (char*)"specflux";
  pitch_methods[0] = (char*)"default";
  pitch_methods[1] = (char*)"schmitt";
  pitch_methods[2] = (char*)"fcomb";
  pitch_methods[3] = (char*)"mcomb";
  pitch_methods[4] = (char*)"yin";
  pitch_methods[5] = (char*)"yinfft";
  Stk::setRawwavePath("/usr/local/lib/stk/rawwaves");
  for (int i = 0; i < harm->nvoices; i++) {
    voiceByName((char*)"Plucked", &harm->instrument_1[i]);
    voiceByName((char*)"Plucked", &harm->instrument_2[i]);
    harm->voicer_1->addInstrument(harm->instrument_1[i]);
    harm->voicer_2->addInstrument(harm->instrument_2[i]);
  }
  for (int i = 0; i < NUM_ONSET_METHODS; i++) {
    harm->onsets[i] = new_aubio_onset(onset_methods[i], harm->bufsize,
     harm->hopsize, harm->samplerate);
    harm->onsets_2[i] = new_aubio_onset(onset_methods[i], harm->bufsize,
     harm->hopsize, harm->samplerate);
  }
  for (int i = 0; i < NUM_PITCH_METHODS; i++) {
    harm->pitches_1[i] = new_aubio_pitch(pitch_methods[i], harm->bufsize * 4,
     harm->hopsize, harm->samplerate);
    harm->pitches_2[i] = new_aubio_pitch(pitch_methods[i], harm->bufsize * 4,
    harm->hopsize, harm->samplerate);
  }
  return (LV2_Handle)harm;
}

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
	Harmonizer* harm = (Harmonizer*)instance;
	switch ((PortIndex)port) {
	case HARMONIZER_ONSET_METHOD:
		harm->onset_method = (float *)data;
    break;
	case HARMONIZER_ONSET_THRESHOLD:
		harm->onset_threshold = (float *)data;
		break;
  case HARMONIZER_SILENCE_THRESHOLD:
    harm->silence_threshold = (float *)data;
    break;
  case HARMONIZER_PITCH_METHOD:
    harm->pitch_method = (float *)data;
    break;
 	case HARMONIZER_PITCH_THRESHOLD:
		harm->pitch_threshold = (float *)data;
		break;
  case HARMONIZER_INPUT_1:
		harm->input_1 = (float *)data;
		break;
	case HARMONIZER_INPUT_2:
		harm->input_2 = (float *)data;
		break;
	case HARMONIZER_OUTPUT_1:
		harm->output_1 = (float *)data;
		break;
	case HARMONIZER_OUTPUT_2:
		harm->output_2 = (float *)data;
		break;
  case HARMONIZER_MIDI_OUT:
    harm->midi_out = (LV2_Atom_Sequence *)data;
    break;
  }
}

static void
activate(LV2_Handle instance)
{
}

static void
deactivate(LV2_Handle instance)
{
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
  Harmonizer *harm = (Harmonizer*)instance;
	const uint32_t capacity = harm->midi_out->atom.size;
  lv2_atom_forge_set_buffer(&harm->forge, (uint8_t*)harm->midi_out, capacity);
  lv2_atom_forge_sequence_head(&harm->forge, &harm->frame, 0);
  const float *input_1  = harm->input_1;
	const float *input_2  = harm->input_2;
	float *const output_1 = harm->output_1;
	float *const output_2 = harm->output_2;
	uint_t i;
  float new_pitch_1;
  float new_pitch_2;
  for (i = 0; i < n_samples; i++) {
    harm->ab_in[0]->data[i] = (smpl_t)input_1[i];
    harm->ab_in[1]->data[i] = (smpl_t)input_2[i];
    output_1[i] = 0.0;
    output_2[i] = 0.0;
  }
  aubio_onset_set_threshold(harm->onsets[(int)*harm->onset_method],
   (float)*harm->onset_threshold);
  aubio_onset_do(harm->onsets[(int)*harm->onset_method],
   harm->ab_in[0], harm->onset_1);
  aubio_onset_set_threshold(harm->onsets_2[(int)*harm->onset_method],
   (float)*harm->onset_threshold);
  aubio_onset_do(harm->onsets_2[(int)*harm->onset_method],
   harm->ab_in[1], harm->onset_2);
  aubio_pitch_do(harm->pitches_1[(int)*harm->pitch_method],
   harm->ab_in[0], harm->ab_out_1);
  aubio_pitch_do(harm->pitches_2[(int)*harm->pitch_method],
   harm->ab_in[1], harm->ab_out_2);
  new_pitch_1 = fvec_get_sample(harm->ab_out_1, 0);
  new_pitch_2 = fvec_get_sample(harm->ab_out_2, 0);
  note_append(harm->note_buffer_1, new_pitch_1);
  note_append(harm->note_buffer_2, new_pitch_2);
  harm->curlevel_1 = aubio_level_detection(harm->ab_in[0],
   *harm->silence_threshold);
  harm->curlevel_2 = aubio_level_detection(harm->ab_in[1],
   *harm->silence_threshold);
  if (harm->curlevel_1 != 1 || harm->curlevel_2 != 1) {
    lv2_log_trace(&harm->logger, "freqs %f %f levels %f %f\n",
     new_pitch_1, new_pitch_2, harm->curlevel_1, harm->curlevel_2);
  }
  if (fvec_get_sample(harm->onset_1, 0)) {
    if (harm->curlevel_1 == 1.) {
      harm->isready_1 = 0;
      send_noteoff_1(harm->curnote_1, 2, harm);
    } else {
      harm->isready_1 = 1;
    }
  } else {
    if (harm->isready_1 > 0)
      harm->isready_1++;
    if (harm->isready_1 == harm->median) {
      send_noteoff_1(harm->curnote_1, 2, harm);
      harm->newnote_1 = get_note(harm->note_buffer_1, harm->note_buffer2_1);
      harm->curnote_1 = harm->newnote_1;
      if (harm->curnote_1 > 0){
        lv2_log_trace(&harm->logger, "NOTE ON: L %f %d\n", harm->curnote_1,
         127+(int)floorf(harm->curlevel_1));
        send_noteon_1(harm->curnote_1, 127+(int)floorf(harm->curlevel_1), harm);
      }
    }
  }
  if (fvec_get_sample(harm->onset_2, 0)) {
    lv2_log_trace(&harm->logger, "Right ONSET\n");
    if (harm->curlevel_2 == 1.) {
      harm->isready_2 = 0;
    //  send_noteon_2(harm->curnote_2, 0, harm);
    } else {
      harm->isready_2 = 1;
    }
  } else {
    if (harm->isready_2 > 0)
      harm->isready_2++;
    if (harm->isready_2 == harm->median) {
  //    send_noteon_2(harm->curnote_2, 0, harm);
      harm->newnote_2 = get_note(harm->note_buffer_2, harm->note_buffer2_2);
      harm->curnote_2 = harm->newnote_2;
      if (harm->curnote_2 > 0){
        lv2_log_trace(&harm->logger, "NOTE ON: R %f %d\n", harm->curnote_2,
         127+(int)floorf(harm->curlevel_2));
        send_noteon_2(harm->curnote_2, 127+(int)floorf(harm->curlevel_2), harm);
      }
    }
  }
  for (i = 0; i < n_samples; i++) {
    output_1[i] = harm->voicer_1->tick();
    output_2[i] = harm->voicer_2->tick();
  }
}

static void
cleanup(LV2_Handle instance)
{
  Harmonizer *harm = (Harmonizer*)instance;
  for (uint i = 0; i < NUM_ONSET_METHODS; i++) {
    //del_aubio_onset(harm->onsets[(char *)onset_methods[i]]);
  }
	free(harm);
}

static const void*
extension_data(const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor = {
	HARMONIZER_URI,
	instantiate,
	connect_port,
	activate,
	run,
	deactivate,
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
	case 0:  return &descriptor;
	default: return NULL;
	}
}
