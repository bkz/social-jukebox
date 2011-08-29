import libspotify_wrapper as libspotify

import os, sys
import json
import getopt
import hashlib
import math
import mimetypes
import logging
import random
import socket
import urlparse
import threading
import urllib2
import time

from collections import defaultdict
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
from SocketServer import ThreadingMixIn

GC_DEAD_CLIENTS_AFTER_SECS = 10
MAX_QUEUE_PER_REQUEST = 5
MAX_QUEUED_SONGS = 100
MIN_PLAY_SEC_PER_SKIP = 10

log = logging.getLogger('spotify')

###########################################################################
# Utilites.
###########################################################################

def parse_track_urls(text):
    """
    Generate list of track URIs from ``text`` which contains newline delimited
    URLs like: http://open.spotify.com/track/76Gmfksb2uBzx5N72DcHT0
    """
    uris = []
    for url in [line.strip() for line in urllib2.unquote(text).splitlines()]:
        tokens = url.split('/') or (None, None)
        if len(tokens) > 2 and tokens[-2] == "track" and len (tokens[-1]) == 22:
            uris.append("spotify:track:%s" % tokens[-1])
    return uris


def make_track_link(uri):
    """
    Return Spotify HTTP URL for track `uri` (i.e. reverse parse_track_urls)
    like: spotify:track:28S2K2KIvnZ9H6AyhRtenm
    """
    tokens = uri.split(':') or (None, None)
    if len(tokens) > 2 and tokens[-2] == "track" and len (tokens[-1]) == 22:
        return "http://open.spotify.com/track/" + tokens[-1]


###########################################################################
# ClientActionQueue.
###########################################################################

class ClientActionQueue(object):
    def __init__(self):
        self.clients = defaultdict(list)
        self.last_access = {}
        self.lock = threading.RLock()

    def gc(self, limit_sec):
        self.lock.acquire()
        now = time.time()
        to_remove = [uuid for (uuid, atime) in self.last_access.iteritems()
                     if (now - atime) > limit_sec]
        for uuid in to_remove:
            del self.clients[uuid]
            del self.last_access[uuid]
        self.lock.release()

    def get(self, uuid):
        self.lock.acquire()
        self.last_access[uuid] = time.time()
        queue = self.clients[uuid]
        self.clients[uuid] = []
        self.lock.release()
        return queue

    def push_all(self, action):
        self.lock.acquire()
        for queue in self.clients.itervalues():
            if action not in queue:
                queue.append(action)
        self.lock.release()


###########################################################################
# JukeboxHandler.
###########################################################################

songs_in_a_row = 0

client_actions = ClientActionQueue()

curr_song_uri = None

curr_album_cover_filename = "static/cover.jpg"

last_skip_timestamp = time.time()

static_dir = "./static"
static_files = ["index.html",
                "sm2/soundmanager2.swf",
                "sm2/soundmanager2_flash9.swf",
                "sm2/soundmanager2-jsmin.js"]


class JukeboxHandler(BaseHTTPRequestHandler):
    def log_error(self, fmt, *args):
        log.error("HTTP ERROR: %s - - [%s] %s" % (self.address_string(),
                                                  self.log_date_time_string(),
                                                  fmt % args))

    def log_message(self, fmt, *args):
        log.debug("HTTP DEBUG: %s - - [%s] %s" % (self.address_string(),
                                                  self.log_date_time_string(),
                                                  fmt % args))

    def serve_error(self, code, message):
        self.send_response(code)
        self.send_header("Content-type", "text/plain")
        self.end_headers()
        self.wfile.write(message)


    def serve_jsonp(self, callback, *args):
        self.send_response(200)
        self.send_header("Content-type", "application/javascript; charset=utf-8")
        self.end_headers()
        if len(args) == 1:
            args = args[0]
        self.wfile.write(callback + "(%s)" % json.dumps(args))


    def do_GET(self):
        (schema, netloc, path, params, query, fragment) = urlparse.urlparse(self.path)
        args = urlparse.parse_qs(query)
        try:
            global client_actions, songs_in_a_row
            global curr_song_uri, curr_album_cover_filename
            global last_skip_timestamp
            if path == "/":
                self.send_response(200)
                self.send_header("Content-type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(open(os.path.join(static_dir, "index.html"), "rt")\
                                     .read().decode("utf-8").encode("utf-8"))
            elif path == "/cover":
                cover_filename = args["filename"][0]
                if cover_filename != curr_album_cover_filename:
                    self.serve_error(400, "invalid album cover")
                else:
                    self.send_response(200)
                    self.send_header("Content-type", "image/jpeg")
                    self.end_headers()
                    self.wfile.write(open(curr_album_cover_filename, "rb").read())
            elif path[1:] in static_files:
                self.send_response(200)
                self.send_header("Content-type",
                                 mimetypes.guess_type(path[1:])[0] or "text/plain")
                self.end_headers()
                self.wfile.write(open(os.path.join(static_dir, path[1:]), "rb").read())
            else:
                uuid, jsonp_callback = args["uuid"][0], args["jsonp_callback"][0]
                if path == "/poll":
                    album_cover_filename = libspotify.album_cover_filename()
                    if album_cover_filename and (album_cover_filename != curr_album_cover_filename):
                        curr_album_cover_filename = album_cover_filename
                    song_uri = libspotify.track_uri()
                    if song_uri and (song_uri != curr_song_uri):
                        curr_song_uri = song_uri
                        songs_in_a_row += 1
                    actions = client_actions.get(uuid)
                    self.serve_jsonp(jsonp_callback,
                                     actions,
                                     libspotify.track_name().decode("utf-8"),
                                     make_track_link(libspotify.track_uri()),
                                     libspotify.queue_length(),
                                     songs_in_a_row,
                                     curr_album_cover_filename)
                    client_actions.gc(GC_DEAD_CLIENTS_AFTER_SECS)
                elif path == "/skip":
                    if (time.time() - last_skip_timestamp) > MIN_PLAY_SEC_PER_SKIP:
                        songs_in_a_row = 0
                        last_skip_timestamp = time.time()
                        libspotify.skip()
                        client_actions.push_all("sync")
                        self.serve_jsonp(jsonp_callback, "ok")
                    else:
                        self.serve_jsonp(jsonp_callback, "no")
                elif path == "/queue":
                    urls = args["urls"][0].strip()
                    uris = parse_track_urls(urls)
                    if len(uris) > MAX_QUEUE_PER_REQUEST:
                        random.shuffle(uris)
                        uris = uris[:MAX_QUEUE_PER_REQUEST]
                    space = MAX_QUEUED_SONGS - libspotify.queue_length()
                    if len(uris) > space:
                        uris = uris[:space]
                    if uris:
                        for uri in uris:
                            libspotify.queue(uri)
                        self.serve_jsonp(jsonp_callback, "ok")
                    else:
                        self.serve_jsonp(jsonp_callback, "no")
                else:
                    self.serve_error(400, "no")
        except KeyError:
            self.serve_error(400, "missing arg")
        except Exception:
            log.exception("Unhandled exception");
            self.serve_error(400, "internal error")



###########################################################################
# ThreadedHTTPServer.
###########################################################################

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    pass


###########################################################################
# Application entry-point.
###########################################################################

if __name__ == "__main__":
    def usage(errmsg=None):
        if errmsg:
            sys.stderr.write("jukebox.py: %s\n" % errmsg)
        else:
            sys.stderr.write("jukebox.py -u <username> -p <password> [-v]\n")
        sys.exit(1)

    try:
        opts, args = getopt.getopt(sys.argv[1:], "u:p:v")
    except getopt.GetoptError, e:
        usage()

    verbosity = logging.INFO
    username, password = (None, None)
    for (o, a) in opts:
        if o == "-u":
            username = a
        elif o == "-p":
            password = a
        elif o == "-v":
            verbosity = logging.DEBUG
        else:
            assert(0)

    if not (username and password):
        usage()

    try:
        logging.basicConfig(format='%(asctime)s - %(levelname)s - %(message)s',
                            level=verbosity)
        threading.Thread(target=libspotify.start, args=(username, password)).start()
        server = ThreadedHTTPServer(("", 8000), JukeboxHandler)
        server.serve_forever()
    except KeyboardInterrupt:
        server.socket.close()
        libspotify.stop()


###########################################################################
# The End.
###########################################################################
