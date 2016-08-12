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
#include <jack/ringbuffer.h>
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
#define RB_SIZE 16384
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
  HARMONIZER_INPUT  = 5,
	HARMONIZER_OUTPUT = 6,
  HARMONIZER_MIDI_OUT = 7
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
  aubio_pitch_t *pitches[NUM_PITCH_METHODS];
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
  const float* input;
	float*       output;
  LV2_Atom_Sequence* midi_out;
  jack_ringbuffer_t* ringbuf;
  smpl_t bufsize;
  smpl_t hopsize;
  char_t *pitch_unit;
  fvec_t *pitch;
  uint_t median;
  uint_t overruns;
  smpl_t newnote;
  uint_t isready;
  smpl_t curnote;
  smpl_t curlevel;
  fvec_t *ab_out;
  fvec_t *ab_in;
  fvec_t *note_buffer;
  fvec_t *note_buffer2;
  fvec_t *onset;
  Instrmnt **instrument;
  string_info voices[NUM_STRINGS];
  Voicer *voicer;
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

smpl_t get_note (fvec_t * note_buffer, fvec_t * note_buffer2) {
  uint_t i;
  for (i = 0; i < note_buffer->length; i++) {
    note_buffer2->data[i] = note_buffer->data[i];
  }
  return fvec_median (note_buffer2);
}

void send_noteon(smpl_t note, smpl_t level, void *usr) {
  Harmonizer *harm = (Harmonizer *)usr;
  if (note > 0) {
    smpl_t midi_note = floor(0.5 + aubio_freqtomidi(note));
    lv2_log_trace(&harm->logger, "noteon note: %f midi_note %f\n", note, midi_note);
    uint8_t event[3];
    event[0] = 0x90;
    event[1] = (uint8_t)midi_note;
    event[2] = (uint8_t)level;
    forge_midimessage(harm, 0, event, 3);
    harm->voicer->noteOn(midi_note, level);
    harm->voicer->noteOn(midi_note + 4, level);
    harm->voicer->noteOn(midi_note + 7, level);
  }
}

void send_noteoff(smpl_t note, smpl_t level, void *usr) {
  Harmonizer *harm = (Harmonizer *)usr;
  smpl_t midi_note = floor(0.5 + aubio_freqtomidi(note));
  uint8_t event[3];
  event[0] = 0x80;
  event[1] = (uint8_t)midi_note;
  event[2] = (uint8_t)level;
  forge_midimessage(harm, 0, event, 3);
  harm->voicer->noteOff(midi_note, level);
  harm->voicer->noteOff(midi_note + 4, level);
  harm->voicer->noteOff(midi_note + 7, level);
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
    double                    rate,
    const char*               bundle_path,
    const LV2_Feature* const* features) {
  Harmonizer* harm = (Harmonizer*)malloc(sizeof(Harmonizer));
  for (int i = 0; i < 2; i++) {
    harm->ringbuf = jack_ringbuffer_create(RB_SIZE);
  }
  for (int i = 0; features[i]; ++i) {
    if (!strcmp (features[i]->URI, LV2_URID__map)) {
      harm->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
      harm->log = (LV2_Log_Log*)features[i]->data;
    }
  }
  lv2_log_logger_init(&harm->logger, harm->map, harm->log);
  if (!harm->map) {
    lv2_log_error (&harm->logger,
     "harmonizer.lv2 error: Host does not support urid:map\n");
    free (harm);
    return NULL;
  }
  lv2_atom_forge_init (&harm->forge, harm->map);
  map_mem_uris (harm->map, &harm->uris);
  harm->samplerate = (float)rate;
  harm->bufsize = 4096;
  harm->hopsize = harm->bufsize / 4;
  harm->median = 6;
  harm->onset = new_fvec(1);
  harm->ab_in = new_fvec(harm->bufsize);
  harm->ab_out = new_fvec(1);
  harm->note_buffer = new_fvec(harm->median);
  harm->note_buffer2 = new_fvec(harm->median);
  harm->nvoices = NUM_STRINGS;
  harm->instrument = (Instrmnt **) calloc(harm->nvoices, sizeof(Instrmnt *));
  harm->voicer = (Voicer *) new Voicer(0.0);
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
    voiceByName((char*)"Plucked", &harm->instrument[i]);
    harm->voicer->addInstrument(harm->instrument[i]);
  }
  for (int i = 0; i < NUM_ONSET_METHODS; i++) {
    harm->onsets[i] = new_aubio_onset(onset_methods[i], harm->bufsize,
     harm->hopsize, harm->samplerate);
  }
  for (int i = 0; i < NUM_PITCH_METHODS; i++) {
    harm->pitches[i] = new_aubio_pitch(pitch_methods[i], harm->bufsize,
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
  case HARMONIZER_INPUT:
		harm->input = (float *)data;
		break;
	case HARMONIZER_OUTPUT:
		harm->output = (float *)data;
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
  const float *input  = harm->input;
  float *const output = harm->output;
  uint_t i;
  float new_pitch;
  uint_t ncalls = 0;
  for (i = 0; i < n_samples; i++) {
    if (jack_ringbuffer_write(harm->ringbuf, (const char*)(input + i), sizeof(smpl_t))
        < sizeof(smpl_t)) {
      harm->overruns++;
    }
    output[i] = 0.0;
  }
  while (jack_ringbuffer_read_space(harm->ringbuf) >= harm->bufsize) {
    ncalls++;
    jack_ringbuffer_read(harm->ringbuf, (char*)harm->ab_in->data, sizeof(smpl_t) *
        harm->bufsize);
  }
  aubio_onset_set_threshold(harm->onsets[(int)*harm->onset_method],
   (float)*harm->onset_threshold);
  aubio_onset_do(harm->onsets[(int)*harm->onset_method],
   harm->ab_in, harm->onset);
  aubio_pitch_do(harm->pitches[(int)*harm->pitch_method],
   harm->ab_in, harm->ab_out);
  new_pitch = fvec_get_sample(harm->ab_out, 0);
  note_append(harm->note_buffer, new_pitch);
  harm->curlevel = aubio_level_detection(harm->ab_in,
   *harm->silence_threshold);
  if (harm->curlevel != 1) {
    lv2_log_trace(&harm->logger, "freqs %f levels %f\n",
     new_pitch, harm->curlevel);
  }
  if (fvec_get_sample(harm->onset, 0)) {
    if (harm->curlevel == 1.0) {
      harm->isready = 0;
      send_noteoff(harm->curnote, 0, harm);
    } else {
      harm->isready = 1;
    }
  } else {
    if (harm->isready > 0)
      harm->isready++;
    if (harm->isready == harm->median) {
      send_noteoff(harm->curnote, 0, harm);
      harm->newnote = get_note(harm->note_buffer, harm->note_buffer2);
      harm->curnote = harm->newnote;
      if (harm->curnote > 0) {
        lv2_log_trace(&harm->logger, "NOTE ON: %f %d\n", harm->curnote,
         127+(int)floorf(harm->curlevel));
        send_noteon(harm->curnote, 127+(int)floorf(harm->curlevel), harm);
      }
    }
  }
  for (i = 0; i < n_samples; i++) {
      output[i] = harm->voicer->tick();
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
