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

#define INCLUDE_MQ_WRITER_TEST

const size_t frameSize = 512;
const size_t charsPerSample = 2; // 2 chars per 16bit PCM sample
const size_t samplesPerFrame = frameSize / charsPerSample;
const int sampleRate = 44100;

#ifdef INCLUDE_MQ_WRITER_TEST
mqd_t write_mqd;
#endif
mqd_t read_mqd;
const char* queueName = "/samples";

unsigned long long timestamp;

#ifdef INCLUDE_MQ_WRITER_TEST
void openMessageQueueForWrite() {
  int flags = O_CREAT | O_WRONLY; // | O_NONBLOCK;
  mode_t perms = S_IRUSR | S_IWUSR;
  struct mq_attr attr;
  attr.mq_maxmsg = 8;
  attr.mq_msgsize = frameSize;
  write_mqd = mq_open(queueName, flags, perms, &attr);
  if (write_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << queueName << "' for write" << std::endl;
    exit(1);
  }
}
#endif

void openMessageQueueForRead() {
  read_mqd = mq_open(queueName, O_RDONLY); // will block
  if (read_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << queueName << "' for read" << std::endl;
    exit(1);
  }
}

#ifdef INCLUDE_MQ_WRITER_TEST
using namespace std::chrono_literals;
void writeFile() {
  mq_unlink(queueName);
  openMessageQueueForWrite();

  std::ifstream file;
  file.open("recordings/Nightsong-signed16b-pcm.raw"); // little-endian
  char *pcmInt16Frame = new char[frameSize];
  // file.seekg(60.0*44100.0*2.0); // 60.0 seconds in, where 2.0 assumes mono 16b signed int
  for(int t = 0; t < 2000; t++) { // 1000 is 5.8s of audio
    std::cout << "Write frame " << t << std::endl;
    std::this_thread::sleep_for(5.814ms); // For 172 frames/s mono

    file.read(pcmInt16Frame, frameSize);
  //  std::cout << "First sample " << *reinterpret_cast<int16_t*>(pcmInt16Frame) << std::endl;

    if (mq_send(write_mqd, pcmInt16Frame, frameSize, 0) == -1) {
      std::cerr << "Can't send'" << std::endl;
      exit(1);
    }
  }
  file.close();
  mq_unlink(queueName);
}
#endif

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
/* std::array<char, MAX_OSC_PACKET_SIZE> oscBuffer; */

size_t makeOscPacket(Gist<float>& gist, char* buffer) {
  OSCPP::Client::Packet packet(buffer, MAX_OSC_PACKET_SIZE);
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

void readMessages() {
  timestamp = 0;

  // wait for an OSC client
  struct sockaddr_storage claddr;
  socklen_t len;
  ssize_t numRead;
  const size_t BUF_SIZE = 16;
  char buf[BUF_SIZE];
  const size_t IS_ADDR_STR_LEN = 4096;
  char addrStr[IS_ADDR_STR_LEN]; // for error output

  len = sizeof(struct sockaddr_storage);
  std::cout << "Wait for OSC client" << std::endl;
  numRead = recvfrom(serverSocketFD, buf, BUF_SIZE, 0, (struct sockaddr *) &claddr, &len);
  if (numRead == -1) {
    std::cerr << "Error from recvFrom" << std::endl;
  }

  // open the MQ for audio frames
  openMessageQueueForRead();

  while(true) {
    struct mq_attr attr;
    if (mq_getattr(read_mqd, &attr) == -1) {
      std::cerr << "Can't fetch attributes for mq '" << queueName << "'" << std::endl;
      exit(1);
    }
    // std::cout << "MQ message size " << attr.mq_msgsize << std::endl;
    char *receivedFrame = new char[attr.mq_msgsize];
  //  std::cout << attr.mq_maxmsg << std::endl;
  //  if (attr.mq_msgsize != frameSize) {
  //    std::cerr << "Message size != frameSize: " << attr.mq_msgsize << std::endl;
  //    exit(1);
  //  }

    unsigned int prio;
    ssize_t sizeRead = mq_receive(read_mqd, receivedFrame, attr.mq_msgsize, &prio);
    if (sizeRead == -1) {
      std::cerr << "Can't receive from mq" << std::endl;
      exit(1);
    }
  //  std::cout << "Size read " << int(sizeRead) << std::endl;
  //  if (sizeRead != frameSize) {
  //    std::cerr << "Received " << sizeRead << std::endl;
  //    exit(1);
  //  }
  //  std::cout << "First byte " << *reinterpret_cast<int16_t*>(receivedFrame) << std::endl;

    float *floatFrame = new float[samplesPerFrame];
    Gist<float> gist(samplesPerFrame, sampleRate);
    for(size_t i = 0; i < frameSize; i += charsPerSample) {
      floatFrame[i / charsPerSample] = *(reinterpret_cast<int16_t*>(receivedFrame + i)); // little-endian int16_t
    }
    //float f = (int)*(receivedFrame) * 256 + (int)*(receivedFrame+1);
    //std::cout << "Big endian float " << f << std::endl;
    //float f2 = (int)*(receivedFrame+1) * 256 + (int)*(receivedFrame);
    //std::cout << "Little endian float " << f2 << std::endl;
    //std::cout << "First float " << int(floatFrame[0]) << std::endl;
    //std::cout << "frame.0: " << (int)receivedFrame[0] << ", frame.1: " << (int)receivedFrame[1] << ", float: " << floatFrame[0] << std::endl;

    gist.processAudioFrame(floatFrame, samplesPerFrame);

    // float pitch = gist.pitch();
    // std::cout << pitch << std::endl;
    // float sd_hwr = gist.spectralDifferenceHWR();
    // float csd = gist.complexSpectralDifference();
    // if (csd > 200000) {
    //   std::cout << csd << std::endl;
    // }

    char* oscBuffer = new char[MAX_OSC_PACKET_SIZE];
    ssize_t bufferSize = makeOscPacket(gist, oscBuffer);
    //std::cout << "Packet " << (int)bufferSize << std::endl;

    if (sendto(serverSocketFD, oscBuffer, bufferSize, 0, (struct sockaddr *) &claddr, len) != bufferSize) {
      std::cerr << "Error sending to " << inetAddressStr((struct sockaddr *) &claddr, len, addrStr, IS_ADDR_STR_LEN) << ": " << strerror(errno) << std::endl;
    }

    timestamp++;
  }
}

// Execute with no args for a reader, else a writer
int main(int argc, char* argv[]) {
#ifdef INCLUDE_MQ_WRITER_TEST
  if (argc > 1) {
    std::cout << "Start writer\n";
    writeFile();
  } else {
#endif
    std::cout << "Start server\n";
    startOscServer();
    std::cout << "Start reader\n";
    readMessages();
#ifdef INCLUDE_MQ_WRITER_TEST
  }
#endif
}

//int main() {
//  std::ifstream file;
//  file.open("recordings/Nightsong-signed16b-pcm.raw"); // little-endian
//  /* file.seekg(4.1*44100.0*2.0); // 60.0 seconds in, where 2.0 assumes mono 16b signed int */
//  char *pcmInt16Frame = new char[frameSize];
//  for(int i = 0; i < 1000; i++) {
//    file.read(pcmInt16Frame, frameSize);
//    float *floatFrame = new float[samplesPerFrame];
//    Gist<float> gist(samplesPerFrame, sampleRate);
//    for(size_t i = 0; i < frameSize; i += charsPerSample) {
//      floatFrame[i / charsPerSample] = *(reinterpret_cast<int16_t*>(pcmInt16Frame + i)); // little-endian int16_t
//    }
//    gist.processAudioFrame(floatFrame, samplesPerFrame);
//    std::cout << i << ": Pitch: " << gist.pitch() << std::endl;
//  }
//}
