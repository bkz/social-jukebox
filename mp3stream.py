import os, sys
import math
import mimetypes
import logging
import socket
import subprocess
import threading
import time

from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
from SocketServer import ThreadingMixIn

NUM_BYTES_PER_STDIN_READ = 4*1024 # Match libspotify 4 kB buffer.
RATE_LIMIT_CLIENT_KB_PER_SEC = 40 # 1 sec MP3 @ 192 kbs ~ 25kB.
RING_BUFFER_NUM_SLOTS = 8

log = logging.getLogger('mp3stream')

###########################################################################
# Platform setup.
###########################################################################

try:
    import msvcrt
    msvcrt.setmode (sys.stdin.fileno(), os.O_BINARY)
    msvcrt.setmode (sys.stdout.fileno(), os.O_BINARY)
except ImportError:
    pass

if sys.platform == "win32":
    time.time = time.clock


###########################################################################
# TokenBucket.
###########################################################################

class TokenBucket(object):
    """
    Token bucket control mechanism which provides a very simple interface for
    limiting resource usage in various scenarious. The bucket is initially
    created with a capacity of C tokens and a fill rate value R which adds a
    token the bucket over time (one token every 1/R second). To schedule
    resource usage of size N simply call the consume(N) method which will take
    care of waiting until the required amount tokens are available. The
    algorithm allows burst of up to C tokens but over the long run the
    troughput is limited to the constant rate R.
    """
    def __init__(self, capacity, fillrate):
        """
        Initialize token bucket with a maximum ``capacity`` and a ``fillrate``
        which specifies how many tokens are added per second to the bucket.
        """
        self.capacity, self.tokens = capacity, capacity
        self.fillrate = float(fillrate)
        self.timestamp = time.time()

    def consume(self, n):
        """
        Remove ``n`` tokens from the bucket. If the bucket contains enough
        tokens the call will return without stalling (burst) else block until
        the required amount of tokens have been generated and consumed. Returns
        True if tokens were consumed without stalling or False if a wait was
        required.
        """
        # Note: We can only fill the bucket to it's maximum capacity even if
        # the request needs more tokens than available so larger requests will
        # have to be completed in multiple runs trough this loop.
        if n <= self.tokens:
            self.tokens -= n
            return True

        while n > 0:
            if n >= self.tokens:
                n -= self.tokens
                self.tokens = 0
            else:
                self.tokens -= n
                n = 0

            expected_waittime = min(self.capacity, n) / self.fillrate
            if expected_waittime > 0:
                time.sleep(expected_waittime)

            now = time.time()
            new_tokens = (now - self.timestamp) * self.fillrate
            self.tokens = min(self.capacity, self.tokens + new_tokens)
            self.timestamp = now
        return False


###########################################################################
# DownloadRateLimiter.
###########################################################################

class DownloadRateLimiter(object):
    """
    Wrapper for TokenBucket for usage in bandwidth throttling scenarios.

    Example:

        Limit downloading 'foo.zip' to 150 kB/s (with a maximum burst of 512 kB).

        rate_limit = DownloadRateLimiter(150, 512)

        for chunk in http('http://cdn.domain.com/files.zip').read():
            target_file.write(chunk)
            rate_limit.checkpoint(len(chunk))

    """
    def __init__(self, rate, burst=None):
        """
        Create instance with ``rate`` specifying the average bandwidth limit in
        kB/s. ``burst`` may be optionally set to a kB value controlling the
        size of the token bucket otherwise it will default to ``rate`` (which
        is the best choice for most usage scenarios).
        """
        self.lock = threading.Lock()
        self.rate = float(rate)
        self.avgrate = 0
        self.samples = []
        self.timestart = time.time()
        self.timestamp = self.timestart
        if burst:
            self.bucket = TokenBucket(burst, rate)
        else:
            self.bucket = TokenBucket(rate, rate)

    def checkpoint(self, delta):
        """
        Report a checkpoint where ``delta`` bytes have been downloaded since
        the last the call, will possibly result in a sleep() for the calling
        thread if bandwidth throttling is needed.
        """
        self.lock.acquire()
        now = time.time()
        # Calculate size of downloaded data (in kB) and time spent since last
        # call, total so far and the total amount of data we expect to fetch.
        delta_kb = delta / 1024
        elapsed_time, delta_time = now - self.timestart, now - self.timestamp

        # Throttle bandwidth usage by consuming N tokens from our bucket
        # matching the amount of data transfered since last call. The bucket
        # will automatically sleep() the required throttle factor to control
        # the bandwidth usage.
        burst = self.bucket.consume(delta_kb)

        # Calculate average download rate (kB/s) by calculating a running mean
        # from a window of the 10 latest rate samples. Don't bother with
        # samples when transmission was temporarily allowed to burst during a
        # small period of time.
        if not burst:
            if delta_time > 0:
                self.samples.append(delta_kb / delta_time)
            if len(self.samples) > 10:
                self.samples = self.samples[-10:]
            if len(self.samples) > 0:
                self.avgrate = (0.9 * self.avgrate) + \
                               (0.1 * sum(self.samples) / len(self.samples))

        # Update variables needed for calculating deltas in future calls.
        self.timestamp = now
        self.lock.release()


###########################################################################
# RingBuffer.
###########################################################################

class RingBuffer(object):
    def __init__(self, quit_event, size):
        self.size = size
        self.data = [None] * size
        self.read_index = 0
        self.write_index = 0
        self.lock = threading.Lock()
        self.update_event = threading.Event()
        self.quit_event = quit_event

    def put(self, data):
        self.lock.acquire()
        log.debug("PUT %s" % self.write_index)
        self.data[self.write_index % self.size] = data
        self.write_index += 1
        self.update_event.set()
        self.lock.release()

    def get(self, n):
        if n is None:
            n = self.read_index
        log.debug("GET %s" % n)
        while (n >= self.write_index) and not quit_event.isSet():
            self.lock.acquire()
            self.update_event.clear()
            self.lock.release()
            log.debug("WAIT r:%s w:%s" % (n, self.write_index))
            self.update_event.wait()
        if quit_event.isSet():
            return (-1, None)
        self.lock.acquire()
        data = self.data[n % self.size]
        assert data is not None
        self.read_index = n
        self.lock.release()
        return (n+1, data)

    def quit(self):
        self.quit_event.set()
        self.update_event.set()


###########################################################################
# Stream producer thread (filter PCM data from stdin via FFMpeg).
###########################################################################

def stdin_reader(quit_event, ring_buffer):
    devnull = os.open(os.devnull, os.O_RDWR)
    cmdline = "ffmpeg -f s16le -ar 44100 -ac 2 -i pipe:0 -f mp3 -ab 192k pipe:1".split()
    ffmpeg  = subprocess.Popen(cmdline, stdin=sys.stdin, stdout=subprocess.PIPE, stderr=devnull)

    while not quit_event.isSet():
        if ffmpeg.poll() is not None:
            log.error("ffmpeg child process terminated")
            break
        # No point in reading more data if we're encoding the stream at a
        # faster rate than the clients are consuming.
        if (ring_buffer.write_index - ring_buffer.read_index) >= ring_buffer.size:
            log.debug("PAUSE w:%s r:%s" % (ring_buffer.write_index,
                                           ring_buffer.read_index))
            time.sleep(0.01)
            continue
        data = ffmpeg.stdout.read(NUM_BYTES_PER_STDIN_READ)
        if data:
            ring_buffer.put(data)


###########################################################################
# StreamMP3Handler.
###########################################################################

CROSSDOMAIN_XML =\
"""
<?xml version="1.0" ?>
<cross-domain-policy>
  <allow-access-from domain="*" />
</cross-domain-policy>
"""

ring_buffer = None

class StreamMP3Handler(BaseHTTPRequestHandler):
    def log_error(self, fmt, *args):
        log.error("HTTP ERROR: %s - - [%s] %s" % (self.address_string(),
                                                  self.log_date_time_string(),
                                                  fmt % args))

    def log_message(self, fmt, *args):
        log.debug("HTTP DEBUG: %s - - [%s] %s" % (self.address_string(),
                                                  self.log_date_time_string(),
                                                  fmt % args))
    def serve_stream(self):
        index = None
        ratelimit = DownloadRateLimiter(RATE_LIMIT_CLIENT_KB_PER_SEC)
        while True:
            index, data = ring_buffer.get(index)
            if data is None:
                log.debug("EXIT")
                break
            else:
                self.wfile.write(data)
            ratelimit.checkpoint(len(data))
            log.debug("AVG: %s kB/s" % ratelimit.avgrate)


    def do_GET(self):
        try:
            if self.path == "/stream.mp3":
                self.send_response(200)
                self.send_header("Content-type", "audio/mpeg")
                self.end_headers()
                self.serve_stream()
            elif self.path == "/crossdomain.xml":
                self.send_response(200)
                self.send_header("Content-type", "text/xml")
                self.end_headers()
                self.wfile.write(CROSSDOMAIN_XML);
            else:
                self.send_response(400)
                self.send_header("Content-type", "text/plain")
                self.end_headers()
                self.wfile.write("no")
        except Exception:
            log.exception("Unhandled exception");


###########################################################################
# ThreadedHTTPServer.
###########################################################################

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    pass


###########################################################################
# Application entry-point.
###########################################################################

if __name__ == "__main__":
    verbosity = logging.INFO
    if "-v" in sys.argv:
        verbosity = logging.DEBUG

    logging.basicConfig(format='%(asctime)s - %(levelname)s - %(message)s',
                        level=verbosity)

    try:
        quit_event = threading.Event()
        ring_buffer = RingBuffer(quit_event, RING_BUFFER_NUM_SLOTS)
        threading.Thread(target=stdin_reader,
                         args=(quit_event, ring_buffer)).start()
        server = ThreadedHTTPServer(("", 8001), StreamMP3Handler)
        server.serve_forever()
    except KeyboardInterrupt:
        ring_buffer.quit()
        server.socket.close()


###########################################################################
# The End.
###########################################################################
