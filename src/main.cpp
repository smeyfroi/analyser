#include <mqueue.h>
#include <fcntl.h>              /* For definition of O_NONBLOCK */
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
using namespace std::chrono_literals;
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
// #include <arpa/inet.h>
#define _BSD_SOURCE   /* To get definitions of NI_MAXHOST and NI_MAXSERV from <netdb.h> */
#include <netdb.h>
#define ADDRSTRLEN (NI_MAXHOST + NI_MAXSERV + 10)
#include <unistd.h>
#include "Gist.h"
#include <oscpp/client.hpp>

#define BACKLOG 50

/* const auto startTime = std::chrono::system_clock::now(); // arbitrary start point for timestamps */

const int sampleRate = 48000; // whatever Jamulus is sending to us

mqd_t read_mqd;
const char* queueName = "/samples";

int lfd;
const char* osc_port = "8000";

void openMessageQueueForRead() {
  read_mqd = mq_open(queueName, O_RDONLY); // will block
  if (read_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << queueName << "' for read" << std::endl;
    exit(1);
  }
  std::cout << "Opened mq for read" << std::endl;
}

// Populates the lfd
void startOscServer() {
  /* Call getaddrinfo() to obtain a list of addresses that we can try binding to */
  struct addrinfo hints;
  struct addrinfo *result;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC; /* Allows IPv4 or IPv6 */
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV; /* Wildcard IP address; service name is numeric */
  if (getaddrinfo(NULL, osc_port, &hints, &result) != 0) {
    std::cerr << "ERR: getaddrinfo" << std::endl;
    exit(1);
  }

  /* Walk through returned list until we find an address structure that can be used to successfully create and bind a socket */
  int optval = 1;
  struct addrinfo *rp;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    lfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (lfd == -1)
      continue;                   /* On error, try next address */
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
      std::cerr << "ERR: setsockopt" << std::endl;
      exit(-1);
    }
    if (bind(lfd, rp->ai_addr, rp->ai_addrlen) == 0)
      break;                      /* Success */
    /* bind() failed: close this socket and try next address */
    close(lfd);
  }
  if (rp == NULL) {
    std::cerr << "ERR: Could not bind socket to any address" << std::endl;
    exit(-1);
  }
  if (listen(lfd, BACKLOG) == -1) {
    std::cerr << "ERR: listen" << std::endl;
    exit(-1);
  }
  freeaddrinfo(result);
  std::cout << "Opened OSC listener" << std::endl;
}

const size_t MAX_OSC_PACKET_SIZE = 512; // safe max is ethernet packet MTU 1500 (minus overhead gives max 1380) https://superuser.com/questions/1341012/practical-vs-theoretical-max-limit-of-tcp-packet-size
const size_t OSC_TERMINATOR_LENGTH = 6;
const char* OSC_TERMINATOR = "[/TCP]";
char* oscBuffer = new char[MAX_OSC_PACKET_SIZE];
/* std::array<char, MAX_OSC_PACKET_SIZE> oscBuffer; */

// Terminate the OSC buffer with a terminator that OpenFrameworks uses
// Use the frameSequence as OSC timestamp, which is not correct, but might be enough
size_t makeOscPacket(int channelId, uint64_t frameSequence, Gist<float>& gist) {
//  const auto now = std::chrono::system_clock::now();
//  unsigned long long timestamp = std::chrono::nanoseconds(now - startTime).count(); // TODO: this should be a 64bit NTP Timestamp
  OSCPP::Client::Packet packet(oscBuffer, MAX_OSC_PACKET_SIZE);
  packet
    //.openBundle(timestamp)
    .openBundle(frameSequence)
      .openMessage("/meta", 1)
        .int32(channelId)
      .closeMessage()
      .openMessage("/time", 3)
        .float32(gist.rootMeanSquare())
        .float32(gist.peakEnergy())
        .float32(gist.zeroCrossingRate())
      .closeMessage()
      .openMessage("/freq", 5)
        .float32(gist.spectralCentroid())
        .float32(gist.spectralCrest())
        .float32(gist.spectralFlatness())
        .float32(gist.spectralRolloff())
        .float32(gist.spectralKurtosis())
      .closeMessage()
      .openMessage("/onset", 5)
        .float32(gist.energyDifference())
        .float32(gist.spectralDifference())
        .float32(gist.spectralDifferenceHWR())
        .float32(gist.complexSpectralDifference())
        .float32(gist.highFrequencyContent())
      .closeMessage()
      .openMessage("/pitch", 1)
        .float32(gist.pitch())
      .closeMessage()
//      .openMessage("/spectrum", OSCPP::Tags::array(gist.getMagnitudeSpectrum().size()))
//        .openArray()
//  for(float x : gist.getMagnitudeSpectrum()) {
//    packet.float32(x)
//  }
//  packet
//        .closeArray()
//      .closeMessage()
//      .openMessage("/mel", OSCPP::Tags::array(gist.getMelFrequencySpectrum().size()))
//        .openArray()
//  for(float x : gist.getMelFrequencySpectrum()) {
//    packet.float32(x)
//  }
//  packet
//        .closeArray()
//      .closeMessage()
      .openMessage("/mfcc", OSCPP::Tags::array(gist.getMelFrequencyCepstralCoefficients().size()));
  for(float x : gist.getMelFrequencyCepstralCoefficients()) {
    packet.float32(x);
  }
  packet
      .closeMessage()
    .closeBundle();
  size_t packet_size = packet.size();
  if (packet_size > MAX_OSC_PACKET_SIZE - OSC_TERMINATOR_LENGTH) {
    std::cerr << "Packet size " << packet_size << " > max of " << (MAX_OSC_PACKET_SIZE - OSC_TERMINATOR_LENGTH) << std::endl;
  }
  memcpy(static_cast<void*>(oscBuffer+packet_size), OSC_TERMINATOR, OSC_TERMINATOR_LENGTH);
  return packet_size + OSC_TERMINATOR_LENGTH;
}

constexpr size_t MAX_MQ_FRAME_SIZE = 2048; // must be at least mq_msgsize
char* receivedFrame = new char[MAX_MQ_FRAME_SIZE];
char* receivedMeta = new char[MAX_MQ_FRAME_SIZE];
constexpr size_t MAX_MQ_FLOAT_FRAME_SIZE = MAX_MQ_FRAME_SIZE / sizeof(float);
float* floatFrame = new float[MAX_MQ_FLOAT_FRAME_SIZE];
constexpr size_t IS_ADDR_STR_LEN = 4096;
char* addrStr = new char[IS_ADDR_STR_LEN]; // for error output

// Copy this from Jamulus jamrecorder.cpp
struct meta_t { int16_t channelId; uint64_t frameSequence; };

void readMessages() {
  // open the MQ for audio frames
  openMessageQueueForRead();

  // Loop to listen for clients and send them analysed audio
  char host[NI_MAXHOST];
  char service[NI_MAXSERV];
  char addrStr[ADDRSTRLEN];
  unsigned int prio;
  while(true) { // handle client connections, serially, forever

    /* Accept a client connection, obtaining client's address */
    struct sockaddr_storage claddr;
    socklen_t addrlen = sizeof(struct sockaddr_storage);
    int cfd = accept(lfd, (struct sockaddr *) &claddr, &addrlen);
    if (cfd == -1) {
      std::this_thread::sleep_for(500ms);
      continue;
    }
    if (getnameinfo((struct sockaddr *) &claddr, addrlen, host, NI_MAXHOST, service, NI_MAXSERV, 0) == 0)
      snprintf(addrStr, ADDRSTRLEN, "(%s, %s)", host, service);
    else
      snprintf(addrStr, ADDRSTRLEN, "(?UNKNOWN?)");
    std::cout << "OSC client connection from " << addrStr << std::endl;

    // Prepare to read from mq
    struct mq_attr attr;
    if (mq_getattr(read_mqd, &attr) == -1) {
      std::cerr << "Can't fetch attributes for mq '" << queueName << "'" << std::endl;
      break;
    }
    if (attr.mq_msgsize > MAX_MQ_FRAME_SIZE) {
      std::cerr << "mq_msgsize " << attr.mq_msgsize << " > MAX_MQ_FRAME_SIZE " << MAX_MQ_FRAME_SIZE << std::endl;
      break;
    }

    // Flush the mq of old messages
    attr.mq_flags = O_NONBLOCK;
    mq_setattr(read_mqd, &attr, NULL);
    ssize_t flushRead = 1;
    while(flushRead > 1) {
      flushRead = mq_receive(read_mqd, receivedMeta, attr.mq_msgsize, &prio);
    }
    attr.mq_flags = 0;
    mq_setattr(read_mqd, &attr, NULL);

    // send OSC messages to one client until it disconnects
    while(true) {

      // First message is the metadata as a meta_t struct
      struct meta_t* meta;
      ssize_t sizeRead = mq_receive(read_mqd, receivedMeta, attr.mq_msgsize, &prio);
      if (sizeRead == sizeof(meta_t)) {
        meta = reinterpret_cast<meta_t*>(receivedMeta);
        // Second message is the audio frame
        sizeRead = mq_receive(read_mqd, receivedFrame, attr.mq_msgsize, &prio);
      }
      // error checking
      if (sizeRead < 200 || sizeRead % 2 == 1) {
        std::cerr << "weird message size " << sizeRead << std::endl;
        continue;
      }

      // samples from Jamulus are int16_t; Gist wants float32; so convert
      int sampleCount = sizeRead / sizeof(int16_t);
      for(size_t i = 0; i < sizeRead; i += sizeof(int16_t)) {
        floatFrame[i / sizeof(int16_t)] = static_cast<float>(*(reinterpret_cast<int16_t*>(receivedFrame + i))); // little-endian int16_t to float32
      }

      Gist<float> gist(sampleCount, sampleRate);
      gist.processAudioFrame(floatFrame, sampleCount);

      ssize_t bufferSize = makeOscPacket(meta->channelId, meta->frameSequence, gist);

      if (write(cfd, oscBuffer, bufferSize) != bufferSize) {
        close(cfd);
        std::cerr << "Disconnect and wait for new connection" << std::endl;
        break; // client went away so go back to waiting for a connection
      }

      // TODO: write to file
    }
  }
}

int main(int argc, char* argv[]) {
  /* Ignore the SIGPIPE signal, so that we find out about broken connection
     errors via a failure from write(). */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    std::cerr << "ERR: signal" << std::endl;
    exit(1);
  }

  std::cout << "Start OSC server\n";
  startOscServer();
  std::cout << "Start pipeline\n";
  readMessages();
}
