#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>
#include <jack/jack.h>
#include <aubio/aubio.h>
#include <aubio/pitch/pitch.h>
#include <aubio/mathutils.h>
#include <Stk.h>
#include <Cubic.h>
#include <Guitar.h>
#include <Instrmnt.h>
#include <Voicer.h>
#include <JCRev.h>
#include "utilities.h"
#include <algorithm>

#define NUM_STRINGS 3

using namespace stk;

typedef jack_default_audio_sample_t sample_t;

struct string_info {
  bool         in_use;
  unsigned int inote;
  string_info() : in_use(false), inote(0) {};
};

struct tick_data {
  int cur_midi_note;
  smpl_t silence_threshold;
  smpl_t curnote;
  smpl_t curlevel;
  fvec_t *ab_out;
  fvec_t *ab_in[2];
  fvec_t *note_buffer;
  fvec_t *note_buffer2;
  jack_port_t *my_output_ports[2];
  jack_port_t *my_input_ports[2];
  jack_client_t *client;
  fvec_t *onset;
  aubio_pitch_t *p;
  aubio_onset_t *o;
  Instrmnt **instrument;
  Guitar *guitar;
  string_info voices[NUM_STRINGS];
  Voicer *voicer;
  StkFloat volume;
  StkFloat t60;
  int samplerate;
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
  Cubic distortion;
  StkFloat feedbackSample;

  tick_data() : 
   silence_threshold(-90), curlevel(0), curnote(0), instrument(0), voicer(0), volume(1.0), t60(2.0),
   nvoices(2), samplerate(48000), current_voice(0),
   channels(2), counter(0), feedbackSample(0.0) {}
};

jack_nframes_t jsample_rate = 48000;

const char *err_buf;
int intval;

fvec_t *pitch;
uint_t median = 6;
smpl_t newnote = 0.;
uint_t isready = 0;

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

void setup_aubio(jack_nframes_t nframes, tick_data *data) {
  data->ab_in[0] = new_fvec(nframes);
  data->ab_in[1] = new_fvec(nframes);
  data->ab_out = new_fvec(1);
  if (median) {
    data->note_buffer = new_fvec (median);
    data->note_buffer2 = new_fvec (median);
  }
  char_t * onset_method = "default";
  smpl_t onset_threshold = 0.; // will be set if != 0.
  char_t * pitch_unit = "default";
  char_t * pitch_method = "default";
  smpl_t pitch_tolerance = 0.; // will be set if != 0.
  int buffer_size = 4 * nframes;
  int hop_size = nframes;
  data->o = new_aubio_onset(onset_method, buffer_size, hop_size, jsample_rate);
  if (onset_threshold != 0.)
    aubio_onset_set_threshold (data->o, onset_threshold);
  data->onset = new_fvec(1);
  data->p = new_aubio_pitch (pitch_method, buffer_size, hop_size, jsample_rate);
  if (pitch_tolerance != 0.)
    aubio_pitch_set_tolerance (data->p, pitch_tolerance);
  if (data->silence_threshold != -90.)
    aubio_pitch_set_silence (data->p, data->silence_threshold);
  if (pitch_unit != NULL)
    aubio_pitch_set_unit (data->p, pitch_unit);
}

int on_jack_buf_size_change(jack_nframes_t nframes, void *usr) {
  tick_data *data;
  data = (tick_data *)usr;
  setup_aubio(nframes, data);
  return nframes;
}

void send_noteon(smpl_t note, smpl_t level, void *usr) {
  tick_data *td;
  td = (tick_data *)usr;
  smpl_t midi_note = floorf(aubio_freqtomidi(note) + 0.5);
  td->voicer->noteOn(midi_note, level);
  td->voicer->noteOn(midi_note+4, level);
  td->voicer->noteOn(midi_note+7, level);
}

int process (jack_nframes_t nframes, void *usr) {
	uint_t i,j;
  sample_t *in[2];
  sample_t *out[2];
  smpl_t new_pitch;
  tick_data *data;
  data = (tick_data *)usr;
  sample_t samples[nframes];
  for (i = 0; i < 2; i++) {
    in[i] = (sample_t *) jack_port_get_buffer(data->my_input_ports[i], nframes);
    out[i] = (sample_t *) jack_port_get_buffer(data->my_output_ports[i], nframes);
    for (j = 0; j < nframes; j++) {
      data->ab_in[i]->data[j] = (smpl_t) in[i][j];
      out[i][j] = 0.0;
    }
  }
  aubio_onset_do(data->o, data->ab_in[0], data->onset);
  aubio_pitch_do(data->p, data->ab_in[0], data->ab_out);
  new_pitch = fvec_get_sample(data->ab_out, 0);
  if(median){
    note_append(data->note_buffer, new_pitch);
  }
  data->curlevel = aubio_level_detection(data->ab_in[0], data->silence_threshold);
  if (fvec_get_sample(data->onset, 0)) {
    /* test for silence */
    if (data->curlevel == 1.) {
      if (median) isready = 0;
      /* send note off */
      send_noteon(data->curnote, 0, data);
    } else {
      if (median) {
        isready = 1;
      } else {
        /* kill old note */
        send_noteon(data->curnote, 0, data);
        /* get and send new one */
        send_noteon(new_pitch, 127+(int)floor(data->curlevel), data);
        data->curnote = new_pitch;
      }
    }
  } else {
    if (median) {
      if (isready > 0)
        isready++;
      if (isready == median)
      {
        /* kill old note */
        send_noteon(data->curnote, 0, data);
        newnote = get_note(data->note_buffer, data->note_buffer2);
        data->curnote = newnote;
        /* get and send new one */
        if (data->curnote>45){
          send_noteon(data->curnote, 127+(int)floor(data->curlevel), data);
        }
      }
    } // if median
  }

  for (i = 0; i < nframes; i++) {
    samples[i] = data->voicer->tick();
  }
  for (i = 0; i < 2; i++) {
    for (j = 0; j < nframes; j++) {
      out[i][j] = samples[j];
    }
  }
	return 0;
}

jack_nframes_t setup_jack(tick_data *data) {
  char *client_name;
  jack_options_t jack_options = JackNullOption;
  jack_status_t status;
  jack_nframes_t buffer_size;
  client_name = (char *) malloc(80 * sizeof(char));
  char *name = "dans_aubio_pitch_detect";
  strcpy(client_name, name);
  printf("making client: '%s'\n", name);
  data->client = jack_client_open(client_name, jack_options, &status);
  if (data->client == NULL) {
    fprintf(stderr, "jack_client_open() failed, "
     "status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf(stderr, "Unable to connect to JACK server\n");
    }
    exit(1);
  }
  if (status & JackServerStarted) {
    fprintf(stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(data->client);
    fprintf(stderr, "unique name `%s' assigned\n", client_name);
  }
  printf("before setting data->inputports to jack_port_register\n");
  data->my_input_ports[0] = jack_port_register (data->client, "in_l",
                                           JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsInput, 0);
  data->my_input_ports[1] = jack_port_register (data->client, "in_r",
                                           JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsInput, 0);
  data->my_output_ports[0] = jack_port_register (data->client, "out_l",
                                           JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsOutput, 0);
  data->my_output_ports[1] = jack_port_register (data->client, "out_r",
                                           JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsOutput, 0);
  printf("after setting data->inputports to jack_port_register\n");
  if ((data->my_input_ports[0] == NULL) ||
      (data->my_input_ports[1] == NULL) ||
      (data->my_output_ports[0] == NULL) ||
      (data->my_output_ports[1] == NULL)) {
    fprintf(stderr, "no more JACK ports available\n");
    exit (1);
  }
  jsample_rate = jack_get_sample_rate(data->client);
  buffer_size = jack_get_buffer_size(data->client);
  Stk::setSampleRate(jsample_rate);
  printf("after setting Stk::setSampleRate(jsample_rate), %d\n", jsample_rate);
  jack_set_buffer_size_callback(data->client, on_jack_buf_size_change, data);
  jack_set_process_callback(data->client, process, data);
  if (jack_activate(data->client)) {
    fprintf(stderr, "cannot activate client");
    exit (1);
  }
  return buffer_size;
}

static void signal_handler(int sig) {
	fprintf(stderr, "signal received, exiting ...\n");
	exit(0);
}

void setup_signal_handler() {
  signal(SIGQUIT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
  signal(SIGINT, signal_handler);
}

int main (int argc, char *argv[]) {
  int i;
  long tag[NUM_STRINGS];
  struct timespec tim, tim2;
  printf("before tick_data declaration\n");
  tick_data data;
  char *instrument_name = "Plucked";
  uint_t force_overwrite = 0;
  Stk::setRawwavePath("/usr/local/lib/stk/rawwaves");
  data.nvoices = NUM_STRINGS;
  printf("we get anywhere?\n");
  data.instrument = (Instrmnt **) calloc(data.nvoices, sizeof(Instrmnt *));
  data.voicer = (Voicer *) new Voicer(0.0);
  for (int i = 0; i < data.nvoices; i++) {
    voiceByName(instrument_name, &data.instrument[i]);
    data.voicer->addInstrument(data.instrument[i]);
  }
  printf("we get just before setup_jack\n");
  int buf_size = setup_jack(&data);
  printf("we get past setup_jack\n");
  setup_aubio(buf_size, &data);
  pitch = new_fvec (1);
  smpl_t base_note = 70.0;
  smpl_t major[data.nvoices];
  major[0] = 0; //C
  major[1] = 4; //E
  major[2] = 7; //G
  major[3] = 12;//C
  major[4] = 16;//E
  major[5] = 19;//E
  tim.tv_sec = 1;
  tim.tv_nsec = 0;
  nanosleep(&tim, &tim2);
  while(1) {
    printf("we get to start of while(1) loop\n");
    for (i = 0; i < data.nvoices; i++) {
      tim.tv_sec = 0;
      tim.tv_nsec = 100000;
      //tag[i] = data.voicer->noteOn(base_note + major[i], 127);
      printf("data midi: %d, freq %f\n", data.cur_midi_note, data.curnote);
      nanosleep(&tim, &tim2);
    }
    tim.tv_sec = 3;
    tim.tv_nsec = 0;
    nanosleep(&tim, &tim2);
    for (i = 0; i < data.nvoices; i++) {
    //   data.voicer->noteOff(tag[i], 0);
    };
    tim.tv_sec = 1;
    tim.tv_nsec = 0;
    nanosleep(&tim, &tim2);
  }
  jack_client_close (data.client);
  return EXIT_SUCCESS;

}
