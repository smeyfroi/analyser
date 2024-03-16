#include <mqueue.h>
#include <fcntl.h>              /* For definition of O_NONBLOCK */
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "Gist.h"
#include <oscpp/client.hpp>

const size_t frameSize = 512;
const size_t charsPerSample = 2; // 2 chars per 16bit PCM sample
const size_t samplesPerFrame = frameSize / charsPerSample;
const int sampleRate = 44100;

mqd_t read_mqd;
const char* queueName = "/samples";

unsigned long long timestamp;

void openMessageQueueForRead() {
  read_mqd = mq_open(queueName, O_RDONLY); // will block
  if (read_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << queueName << "' for read" << std::endl;
    exit(1);
  }
  std::cout << "Opened mq for read" << std::endl;
}

int inetPassiveSocket(const char *service, int type) {
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sfd, s;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
    hints.ai_socktype = type;
    hints.ai_family = AF_UNSPEC;        /* Allows IPv4 or IPv6 */
    hints.ai_flags = AI_PASSIVE;        /* Use wildcard IP address */
    s = getaddrinfo(NULL, service, &hints, &result);
    if (s != 0)
        return -1;
    /* Walk through returned list until we find an address structure
       that can be used to successfully create and bind a socket */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1)
            continue;                   /* On error, try next address */
        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;                      /* Success */
        /* bind() failed: close this socket and try next address */
        close(sfd);
    }
    freeaddrinfo(result);
    return (rp == NULL) ? -1 : sfd;
}

const char* PORT = "8000";
int serverSocketFD;
void startOscServer() {
  serverSocketFD = inetPassiveSocket(PORT, SOCK_DGRAM);
  if (serverSocketFD == -1) {
    std::cerr << "Could not create server socket " << errno;
    exit(-1);
  }
  std::cout << "Opened server socket " << PORT << std::endl;
}

const size_t MAX_OSC_PACKET_SIZE = 512;
char* oscBuffer = new char[MAX_OSC_PACKET_SIZE];
/* std::array<char, MAX_OSC_PACKET_SIZE> oscBuffer; */

size_t makeOscPacket(Gist<float>& gist) {
  OSCPP::Client::Packet packet(oscBuffer, MAX_OSC_PACKET_SIZE);
  packet
    .openBundle(timestamp)
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
      // missing FFT magnitude spectrum, Mel-frequency representations
    .closeBundle();
  size_t packet_size = packet.size();
  if (packet_size > MAX_OSC_PACKET_SIZE) {
    std::cerr << "Packet size " << packet_size << " > max of " << MAX_OSC_PACKET_SIZE << std::endl;
  }
  return packet_size;
}

/* Given a socket address in 'addr', whose length is specified in
   'addrlen', return a null-terminated string containing the host and
   service names in the form "(hostname, port#)". The string is
   returned in the buffer pointed to by 'addrStr', and this value is
   also returned as the function result. The caller must specify the
   size of the 'addrStr' buffer in 'addrStrLen'. */
char* inetAddressStr(const struct sockaddr *addr, socklen_t addrlen, char *addrStr, int addrStrLen) {
    char host[NI_MAXHOST], service[NI_MAXSERV];
    if (getnameinfo(addr, addrlen, host, NI_MAXHOST, service, NI_MAXSERV, NI_NUMERICSERV) == 0)
        snprintf(addrStr, addrStrLen, "(%s, %s)", host, service);
    else
        snprintf(addrStr, addrStrLen, "(?UNKNOWN?)");
    return addrStr;
}

constexpr size_t MAX_MQ_FRAME_SIZE = 520;
char *receivedFrame = new char[MAX_MQ_FRAME_SIZE];
constexpr size_t MAX_MQ_FLOAT_FRAME_SIZE = MAX_MQ_FRAME_SIZE / 4; // float32 is 4 chars
float *floatFrame = new float[MAX_MQ_FLOAT_FRAME_SIZE];
constexpr size_t IS_ADDR_STR_LEN = 4096;
char *addrStr = new char[IS_ADDR_STR_LEN]; // for error output

void readMessages() {
  timestamp = 0;

  struct sockaddr_storage claddr;
  socklen_t len;
  ssize_t numRead;
  const size_t BUF_SIZE = 16; // max OSC client ACK size
  char buf[BUF_SIZE]; // to receive OSC client ACK

  // open the MQ for audio frames
  openMessageQueueForRead();

  len = sizeof(struct sockaddr_storage);
  std::cout << "Wait for OSC client" << std::endl;
  numRead = recvfrom(serverSocketFD, buf, BUF_SIZE, 0, (struct sockaddr *) &claddr, &len);
  if (numRead == -1) {
    std::cerr << "Error from recvFrom" << std::endl;
  }

  while(true) {
    struct mq_attr attr;
    if (mq_getattr(read_mqd, &attr) == -1) {
      std::cerr << "Can't fetch attributes for mq '" << queueName << "'" << std::endl;
      exit(1);
    }

    unsigned int prio;
    ssize_t sizeRead = mq_receive(read_mqd, receivedFrame, attr.mq_msgsize, &prio);
    if (sizeRead == -1) {
      std::cerr << "Can't receive from mq" << std::endl;
      exit(1);
    }

    // TODO: Can we reinterpret cast the entire buffer instead of copying here?
    Gist<float> gist(samplesPerFrame, sampleRate);
    for(size_t i = 0; i < frameSize; i += charsPerSample) {
      floatFrame[i / charsPerSample] = *(reinterpret_cast<int16_t*>(receivedFrame + i)); // little-endian int16_t
    }

    gist.processAudioFrame(floatFrame, samplesPerFrame);

    ssize_t bufferSize = makeOscPacket(gist);

    if (sendto(serverSocketFD, oscBuffer, bufferSize, 0, (struct sockaddr *) &claddr, len) != bufferSize) {
      std::cerr << "Error sending to " << inetAddressStr((struct sockaddr *) &claddr, len, addrStr, IS_ADDR_STR_LEN) << ": " << strerror(errno) << std::endl;
      // TODO: does this mean the client went away? If so we need to go back to a waiting state
    }

    timestamp++;
  }
}

int main(int argc, char* argv[]) {
    std::cout << "Start server\n";
    startOscServer();
    std::cout << "Start reader\n";
    readMessages();
}
