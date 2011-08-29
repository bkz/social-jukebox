#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <queue>
#include <deque>

/////////////////////////////////////////////////////////////////////////////
// Third-party libraries.
/////////////////////////////////////////////////////////////////////////////

#include "libspotify/api.h"

#include "SDL/SDL.h"
#include "SDL/SDL_audio.h"
#include "SDL/SDL_thread.h"

#ifdef _MSC_VER
    #include <windows.h>
    #include <fcntl.h>
    #include <io.h>

    static void platform_init()
    {
        srand(time(0));
        setvbuf(stdout, NULL, _IONBF, 0);
        _setmode(_fileno(stdout), _O_BINARY);

    }
#else
    static void platform_init()
    {
        srand(time(0));
        setvbuf(stdout, NULL, _IONBF, 0);
    }
#endif

#ifdef _MSC_VER
    #pragma comment(lib, "SDL.lib")
    #pragma comment(lib, "SDLmain.lib")
    #pragma comment(lib, "libspotify.lib")
#endif

/////////////////////////////////////////////////////////////////////////////
// Global variables.
/////////////////////////////////////////////////////////////////////////////

/// Global quit flag.
static bool g_quit = false;

/// Synchronization mutex for the waking up the main thread.
static SDL_mutex* g_notify_mutex = NULL;

/// Synchronization condition variable for waking up the main thread.
static SDL_cond* g_notify_cond = NULL;

/// Synchronization variable telling the main thread to process events.
static bool g_notify_do = false;

/// Non-zero when a track has ended and while a new track hasn't started.
static bool g_playback_done = false;

/// Global session config.
static sp_session_config g_spconfig;

/// Callbacks for global session.
static sp_session_callbacks g_session_callbacks;

/// Global libspotify session handle.
static sp_session *g_session = NULL;

/////////////////////////////////////////////////////////////////////////////
// PCM audio bridge (pipe raw data to stdout).
/////////////////////////////////////////////////////////////////////////////

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

enum {
    FADE_IN_AUDIO_DURATION_SECS = 10,
};

enum {
    AUDIO_SAMPLE_SIZE = sizeof(uint16_t),
    AUDIO_SAMPLE_RATE = 44100,
    AUDIO_CHANNELS = 2,
    AUDIO_BUFFER_SAMPLES = 1024,
    AUDIO_BUFFER_SIZE = (AUDIO_CHANNELS * AUDIO_BUFFER_SAMPLES * AUDIO_SAMPLE_SIZE),
};

enum {
    MAX_AUDIO_QUEUE_SIZE = AUDIO_SAMPLE_RATE / AUDIO_BUFFER_SAMPLES
};

/// Handle of currently playing track.
static sp_track* g_current_track =  NULL;

/// Synchronization for jukebox queue;
static SDL_mutex* g_jukebox_mutex = NULL;

/// Jukebox tracks queue.
static std::deque<std::string> g_jukebox_queue;

/// Jukebox current track descriptive string (track - artist).
static std::string g_jukebox_track_name;

/// Jukebox current track album art -> static/cache/%base32%.jpg
static std::string g_jukebox_album_cover_filename;

/// Jukebox curren track Spotify URI.
static std::string g_jukebox_track_uri;

/// Shared audio queue for pushing data between libspotify and SDL/stdout
static std::queue<uint8_t*> g_audio_queue;

/// Synchronization mutex for SDL audio buffering
static SDL_mutex* g_audio_mutex;

/// Synchronization condition variable for the audio thread
static SDL_cond* g_audio_cond;

static int pcm_audio_pipe_thread(void* userdata)
{
    while (!g_quit)
    {
        uint8_t* chunk = NULL;

        if (g_audio_queue.size() > 0)
        {
            SDL_mutexP(g_audio_mutex);
            chunk = g_audio_queue.front();
            g_audio_queue.pop();
            SDL_mutexV(g_audio_mutex);
        }
        else
        {
            SDL_mutexP(g_audio_mutex);
            SDL_CondWait(g_audio_cond, g_audio_mutex);
            SDL_mutexV(g_audio_mutex);
            continue;
        }

        fwrite(chunk, AUDIO_BUFFER_SIZE, 1, stdout);
        fflush(stdout);

        delete[] chunk;
    }

    return 0;
}

static int pcm_audio_delivery(sp_session *sess, const sp_audioformat *format,
                              const void *frames, int num_frames)
{
    SDL_mutexP(g_audio_mutex);

    if (num_frames == 0)
    {
        SDL_mutexV(g_audio_mutex);
        return 0;
    }

    if (g_audio_queue.size() > MAX_AUDIO_QUEUE_SIZE)
    {
        SDL_mutexV(g_audio_mutex);
        return 0;
    }

    assert(format->sample_rate == AUDIO_SAMPLE_RATE);
    assert(format->channels == AUDIO_CHANNELS);
    assert(format->sample_type == SP_SAMPLETYPE_INT16_NATIVE_ENDIAN);

    if (num_frames > AUDIO_BUFFER_SAMPLES)
        num_frames = AUDIO_BUFFER_SAMPLES;

    uint8_t* chunk = new uint8_t[AUDIO_BUFFER_SIZE];
    memset(chunk, 0, AUDIO_BUFFER_SIZE);
    memcpy(chunk, frames, num_frames * AUDIO_SAMPLE_SIZE * AUDIO_CHANNELS);
    g_audio_queue.push(chunk);

    SDL_CondSignal(g_audio_cond);
    SDL_mutexV(g_audio_mutex);

    return num_frames;
}

static void pcm_audio_queue_flush()
{
    SDL_mutexP(g_audio_mutex);

    while (g_audio_queue.size() > 0)
    {
        uint8_t* chunk = g_audio_queue.front();
        delete[] chunk;
        g_audio_queue.pop();
    }

    SDL_mutexV(g_audio_mutex);
}

/////////////////////////////////////////////////////////////////////////////
// Jukebox playback and queue managment.
/////////////////////////////////////////////////////////////////////////////

static std::string get_current_track_description()
{
    std::string r = std::string(sp_track_name(g_current_track));

    r += " - ";
    for (int i = 0; i < sp_track_num_artists(g_current_track); i++)
    {
        if (i > 0)
            r += ", ";

        sp_artist* artist = sp_track_artist(g_current_track, i);

        r += sp_artist_name(artist);
    }

    return r;
}

static void track_play()
{
retry_play:

    if (!g_current_track)
    {
        SDL_mutexP(g_jukebox_mutex);

        if (g_jukebox_queue.size() == 0)
        {
            static const char* default_songs[] = {
                "spotify:track:0IzJ11BMPGG9AQ09wybJw7", // PRINCE!
                "spotify:track:7kT93GKSimAkCNBouOW9uO", // PRINCE!
                "spotify:track:28S2K2KIvnZ9H6AyhRtenm", // PRINCE!
                "spotify:track:6lOPbNUxDvkUkWpJLebgs7", // PRINCE!
                "spotify:track:2uYCwZTphVUfQ4eEsz7yDO", // PRINCE!
            };

            static size_t num_default_songs = sizeof(default_songs) / sizeof(char*);

            static size_t n = 0;

            g_jukebox_queue.push_back(default_songs[n++]);

            if (n == num_default_songs) n = 0;
        }

        while (g_jukebox_queue.size() > 0)
        {
            std::string uri = g_jukebox_queue.front();
            g_jukebox_queue.pop_front();

            sp_link* link = sp_link_create_from_string(uri.c_str());

            if (!link)
            {
                fprintf(stderr, "libspotify-wrapper: uri error parsing %s\n", uri.c_str());
                continue;
            }

            g_current_track = sp_link_as_track(link);
            sp_track_add_ref(g_current_track);
            sp_link_release(link);

            g_jukebox_track_uri = uri;
            break;
        }

        SDL_mutexV(g_jukebox_mutex);
    }

    if (!g_current_track)
    {
        fprintf(stderr, "libspotify-wrapper: failed to fetch track from queue\n");

        return;
    }

    sp_error error = sp_track_error(g_current_track);

    if (error == SP_ERROR_OK)
    {
        SDL_mutexP(g_jukebox_mutex);
        g_jukebox_track_name = get_current_track_description();
        SDL_mutexV(g_jukebox_mutex);

        fprintf(stderr, "libspotify-wrapper: playing track -> %s\n", g_jukebox_track_name.c_str());

        error = sp_session_player_load(g_session, g_current_track);

        if (error == SP_ERROR_OK)
        {
            sp_session_player_play(g_session, true);
        }
        else
        {
            fprintf(stderr, "libspotify-wrapper: track not playable -> %s\n", sp_error_message(error));

            sp_track_release(g_current_track);
            g_current_track = NULL;

            fprintf(stderr, "libspotify-wrapper: skipping track\n");
            goto retry_play;
        }
    }
    else if (error == SP_ERROR_IS_LOADING || error == SP_ERROR_RESOURCE_NOT_LOADED)
    {
        fprintf(stderr, "libspotify-wrapper: track loading\n");
    }
    else
    {
        fprintf(stderr, "libspotify-wrapper: track error %s\n", sp_error_message(error));

        sp_track_release(g_current_track);
        g_current_track = NULL;
    }
}

static void track_ended()
{
    sp_session_player_play(g_session, false);
    sp_session_player_unload(g_session);

    pcm_audio_queue_flush();

    sp_track_release(g_current_track);
    g_current_track = NULL;

    track_play();
}

/////////////////////////////////////////////////////////////////////////////
// Spotify album art callback.
/////////////////////////////////////////////////////////////////////////////

// Base32 implementation
//
// Copyright 2010 Google Inc.
// Author: Markus Gutschke
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

int base32_encode(const uint8_t *data, int length, uint8_t *result,
                  int bufSize) {
  if (length < 0 || length > (1 << 28)) {
    return -1;
  }
  int count = 0;
  if (length > 0) {
    int buffer = data[0];
    int next = 1;
    int bitsLeft = 8;
    while (count < bufSize && (bitsLeft > 0 || next < length)) {
      if (bitsLeft < 5) {
        if (next < length) {
          buffer <<= 8;
          buffer |= data[next++] & 0xFF;
          bitsLeft += 8;
        } else {
          int pad = 5 - bitsLeft;
          buffer <<= pad;
          bitsLeft += pad;
        }
      }
      int index = 0x1F & (buffer >> (bitsLeft - 5));
      bitsLeft -= 5;
      result[count++] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"[index];
    }
  }
  if (count < bufSize) {
    result[count] = '\000';
  }
  return count;
}

static const char* get_album_art_filename(const void* image_id)
{
    char base32_id[512] = {0};
    base32_encode(
        (const uint8_t*)image_id,
        strlen((const char*)(image_id)),
        (uint8_t*)base32_id,
        sizeof(base32_id));

    static std::string filename;

    filename  = "static/cache/";
    filename += base32_id;
    filename += ".jpg";

    return filename.c_str();
}

static bool have_album_art(const byte* image_id)
{
    bool result = false;

    const char* filename = get_album_art_filename(image_id);

    FILE *fp = fopen(filename, "rb");

    if (fp)
    {
        result = true;
        fclose(fp);
    }

    return result;
}

static void save_album_art(sp_image* image)
{
    if (sp_image_format(image) == SP_IMAGE_FORMAT_JPEG)
    {
        size_t size = 0;
        const void* data = sp_image_data(image, &size);

        const char* filename = get_album_art_filename(sp_image_image_id(image));

        if (data && size > 0)
        {
            FILE *fp = fopen(filename, "wb");
            if (fp)
            {
                fwrite(data, size, 1, fp);

                g_jukebox_album_cover_filename = filename;

                fprintf(stderr, "libspotify-wrapper: saved album art %s\n", filename);

                fclose(fp);
            }
            else
            {
                fprintf(stderr, "libspotify-wrapper: failed to open %s\n", filename);
            }
        }
        else
        {
            fprintf(stderr, "libspotify-wrapper: skipping invalid album art\n");
        }
    }
    else
    {
        fprintf(stderr, "libspotify-wrapper: failed to save album art, not JPEG\n");
    }
}

static void SP_CALLCONV album_art_callback(sp_image *image, void *userdata)
{
    save_album_art(image);
    sp_image_release(image);
}

static void fetch_album_art()
{
    g_jukebox_album_cover_filename = "";

    const byte* image_id = sp_album_cover(sp_track_album(g_current_track));

    if (have_album_art(image_id))
    {
        g_jukebox_album_cover_filename = get_album_art_filename(image_id);
    }
    else
    {
        sp_image* image = sp_image_create(g_session, image_id);
        if (image)
        {
            if (sp_image_is_loaded(image))
            {
                save_album_art(image);
                sp_image_release(image);
            }
            else
            {
                sp_image_add_load_callback(image, album_art_callback, NULL);
            }
        }
    }
}

/////////////////////////////////////////////////////////////////////////////
// Spotify API callbacks.
/////////////////////////////////////////////////////////////////////////////

static void SP_CALLCONV play_token_lost(sp_session *sess)
{
    fprintf(stderr, "libspotify-wrapper: play token lost\n");
}

static void SP_CALLCONV logged_in(sp_session *sess, sp_error error)
{
    if (error == SP_ERROR_OK)
    {
        track_play();
    }
    else
    {
        fprintf(stderr, "libspotify-wrapper: login failed -> %s\n", sp_error_message(error));
    }
}

static void SP_CALLCONV logged_out(sp_session *session)
{
    fprintf(stderr, "libspotify-wrapper: logged out\n");
}

static void SP_CALLCONV start_playback(sp_session *session)
{
    fprintf(stderr, "libspotify-wrapper: start playback\n");
}

static void SP_CALLCONV stop_playback(sp_session *session)
{
    fprintf(stderr, "libspotify-wrapper: stop playback\n");
}

static void SP_CALLCONV get_audio_buffer_stats(sp_session *session, sp_audio_buffer_stats *stats)
{
    SDL_mutexP(g_audio_mutex);
    stats->samples = g_audio_queue.size() * AUDIO_BUFFER_SAMPLES;
    stats->stutter = 0;
    SDL_mutexV(g_audio_mutex);
}

static void SP_CALLCONV end_of_track(sp_session *sess)
{
    fprintf(stderr, "libspotify-wrapper: end of track\n");

    SDL_mutexP(g_notify_mutex);
    g_playback_done = true;
    SDL_CondSignal(g_notify_cond);
    SDL_mutexV(g_notify_mutex);
}

static void SP_CALLCONV metadata_updated(sp_session *sess)
{
    fprintf(stderr, "libspotify-wrapper: metadata updated\n");

    fetch_album_art();

    track_play();
}

static int SP_CALLCONV music_delivery(sp_session *sess, const sp_audioformat *format,
                                      const void *frames, int num_frames)
{
    return pcm_audio_delivery(sess, format, frames, num_frames);
}

static void SP_CALLCONV notify_main_thread(sp_session *sess)
{
    SDL_mutexP(g_notify_mutex);
    g_notify_do = true;
    SDL_CondSignal(g_notify_cond);
    SDL_mutexV(g_notify_mutex);
}

static void SP_CALLCONV connection_error(sp_session *session, sp_error error)
{
    if (error != SP_ERROR_OK)
    {
        fprintf(stderr, "libspotify-wrapper: connection error -> %s\n", sp_error_message(error));
    }
}

static void SP_CALLCONV streaming_error(sp_session *session, sp_error error)
{
    if (error != SP_ERROR_OK)
    {
        fprintf(stderr, "libspotify-wrapper: connection error -> %s\n", sp_error_message(error));
    }
}

static void init_session_callbacks()
{
    memset(&g_session_callbacks, 0, sizeof(g_session_callbacks));

    g_session_callbacks.play_token_lost        = play_token_lost;
    g_session_callbacks.logged_in              = logged_in;
    g_session_callbacks.logged_out             = logged_out;

    g_session_callbacks.start_playback         = start_playback;
    g_session_callbacks.stop_playback          = stop_playback;
    g_session_callbacks.get_audio_buffer_stats = get_audio_buffer_stats;

    g_session_callbacks.end_of_track           = end_of_track;
    g_session_callbacks.metadata_updated       = metadata_updated;
    g_session_callbacks.music_delivery         = music_delivery;

    g_session_callbacks.notify_main_thread     = notify_main_thread;
    g_session_callbacks.connection_error       = connection_error;
    g_session_callbacks.streaming_error        = streaming_error;

    g_session_callbacks.log_message            = NULL;
    g_session_callbacks.message_to_user        = NULL;
    g_session_callbacks.userinfo_updated       = NULL;
    g_session_callbacks.offline_status_updated = NULL;
};

static void init_session_config()
{
    memset(&g_spconfig, 0, sizeof(g_spconfig));

    FILE* fp = fopen("spotify.key", "rb");
    if (!fp)
    {
        fprintf(stderr, "libspotify-wrapper: missing application spotify.key file\n");
    }

    fseek(fp, 0, SEEK_END);
    size_t len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *appkey = new char[len+1];
    memset(appkey, 0, len+1);
    fread(appkey, len, 1, fp);
    fclose(fp);

    g_spconfig.api_version          = SPOTIFY_API_VERSION;
    g_spconfig.cache_location       = "temp";
    g_spconfig.settings_location    = "temp";
    g_spconfig.application_key      = appkey;
    g_spconfig.application_key_size = len;
    g_spconfig.user_agent           = "libspotify-pcm";
    g_spconfig.callbacks            = &g_session_callbacks;
};

/////////////////////////////////////////////////////////////////////////////
// Wrapper API (run in main thread for debugging).
/////////////////////////////////////////////////////////////////////////////

static const char* spotify_album_cover_filename()
{
    static std::string storage;

    SDL_mutexP(g_jukebox_mutex);

    storage = g_jukebox_album_cover_filename;

    SDL_mutexV(g_jukebox_mutex);

    return storage.c_str();
}

static const char* spotify_track_uri()
{
    static std::string storage;

    SDL_mutexP(g_jukebox_mutex);

    storage = g_jukebox_track_uri;

    SDL_mutexV(g_jukebox_mutex);

    return storage.c_str();
}

static const char* spotify_track_name()
{
    static std::string storage;

    SDL_mutexP(g_jukebox_mutex);

    storage = g_jukebox_track_name;

    SDL_mutexV(g_jukebox_mutex);

    return storage.c_str();
}

static int spotify_queue_length()
{
    SDL_mutexP(g_jukebox_mutex);

    int n = g_jukebox_queue.size();

    SDL_mutexV(g_jukebox_mutex);

    return n;
}

static int spotify_queue(const char* uri)
{
    SDL_mutexP(g_jukebox_mutex);

    g_jukebox_queue.push_back(std::string(uri));

    std::random_shuffle(g_jukebox_queue.begin(), g_jukebox_queue.end());

    fprintf(stderr, "libspotify-wrapper: queuing track %s (%d)\n", uri, g_jukebox_queue.size());

    int n = g_jukebox_queue.size();

    SDL_mutexV(g_jukebox_mutex);

    notify_main_thread(g_session);

    return n;
}

static int spotify_skip()
{
    fprintf(stderr, "libspotify-wrapper: skipping track\n");

    end_of_track(g_session);

    return 0;
}

static int spotify_stop()
{
    g_quit = 1;

    end_of_track(g_session);

    return 0;
}

static int spotify_start(const char* username, const char* password)
{
    platform_init();

    g_audio_mutex = SDL_CreateMutex();
    g_audio_cond = SDL_CreateCond();
    SDL_CreateThread(pcm_audio_pipe_thread, NULL);

    init_session_callbacks();
    init_session_config();

    sp_error error = sp_session_create(&g_spconfig, &g_session);

    if (error != SP_ERROR_OK)
    {
        fprintf(stderr, "libspotify-wrapper: unable to create session -> %s\n", sp_error_message(error));
        exit(1);
    }

    g_notify_mutex = SDL_CreateMutex();
    g_notify_cond = SDL_CreateCond();
    g_jukebox_mutex = SDL_CreateMutex();

    sp_session_login(g_session, username, password);
    sp_session_preferred_bitrate(g_session, SP_BITRATE_320k);

    SDL_mutexP(g_notify_mutex);

    int next_timeout = 0;

    while (!g_quit)
    {
        if (next_timeout == 0)
        {
            while(!g_notify_do && !g_playback_done)
                SDL_CondWait(g_notify_cond, g_notify_mutex);
        }
        else
        {
            SDL_CondWaitTimeout(g_notify_cond, g_notify_mutex, next_timeout);
        }

        g_notify_do = false;
        SDL_mutexV(g_notify_mutex);

        if (g_playback_done)
        {
            track_ended();
            g_playback_done = false;
        }

        do
        {
            sp_session_process_events(g_session, &next_timeout);
        }
        while (next_timeout == 0);

        SDL_mutexP(g_notify_mutex);
    }

    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Python API (with background thread).
/////////////////////////////////////////////////////////////////////////////

#include <Python.h>

/// Main wrapper thread handle.
static SDL_Thread* g_wrapper_thread = NULL;

struct WrapperThreadArgs
{
    WrapperThreadArgs(const std::string& u, const std::string& p)
        : username(u), password(p) {}
    std::string username;
    std::string password;
};

static int wrapper_main_thread(void* userdata)
{
    WrapperThreadArgs* args = static_cast<WrapperThreadArgs*>(userdata);

    spotify_start(args->username.c_str(), args->password.c_str());

    return 0;
}

static PyObject* py_spotify_spotify_album_cover_filename(PyObject *self, PyObject *args)
{
    return Py_BuildValue("s", spotify_album_cover_filename());
}

static PyObject* py_spotify_track_uri(PyObject *self, PyObject *args)
{
    return Py_BuildValue("s", spotify_track_uri());
}

static PyObject* py_spotify_track_name(PyObject *self, PyObject *args)
{
    return Py_BuildValue("s", spotify_track_name());
}

static PyObject* py_spotify_queue_length(PyObject *self, PyObject *args)
{
    return PyInt_FromLong(spotify_queue_length());
}

static PyObject* py_spotify_queue(PyObject *self, PyObject *args)
{
    char *uri= NULL;

    if (!PyArg_ParseTuple(args, "s", &uri))
        return NULL;

    return PyInt_FromLong(spotify_queue(uri));
}

static PyObject* py_spotify_skip(PyObject *self, PyObject *args)
{
    return PyInt_FromLong(spotify_skip());
}

static PyObject* py_spotify_stop(PyObject *self, PyObject *args)
{
    spotify_stop();

    int status = 0;

    fprintf(stderr, "libspotify-wrapper: waiting for main thread to exit\n");

    SDL_WaitThread(g_wrapper_thread, &status);

    return PyInt_FromLong(status);
}

static PyObject* py_spotify_start(PyObject *self, PyObject *args)
{
    char *username = NULL;
    char *password = NULL;

    if (!PyArg_ParseTuple(args, "ss", &username,  &password))
        return NULL;

    g_wrapper_thread = SDL_CreateThread(wrapper_main_thread,
        new WrapperThreadArgs(username, password));

    return PyInt_FromLong(g_wrapper_thread != NULL);
}

static PyMethodDef methods[] = {
    {"album_cover_filename" , py_spotify_spotify_album_cover_filename,    METH_VARARGS, ""},
    {"track_uri"            , py_spotify_track_uri,                       METH_VARARGS, ""},
    {"track_name"           , py_spotify_track_name,                      METH_VARARGS, ""},
    {"queue_length"         , py_spotify_queue_length,                    METH_VARARGS, ""},
    {"queue"                , py_spotify_queue,                           METH_VARARGS, ""},
    {"skip"                 , py_spotify_skip,                            METH_VARARGS, ""},
    {"stop"                 , py_spotify_stop,                            METH_VARARGS, ""},
    {"start"                , py_spotify_start,                           METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initlibspotify_wrapper(void)
{
    (void) Py_InitModule("libspotify_wrapper", methods);
}

/////////////////////////////////////////////////////////////////////////////
// Application entry-point for standalone debugging.
/////////////////////////////////////////////////////////////////////////////

/*
int main(int argc, char **argv)
{
    spotify_start("bkz", "hendrix");

    return 0;
}
*/

/////////////////////////////////////////////////////////////////////////////
// The End.
/////////////////////////////////////////////////////////////////////////////
