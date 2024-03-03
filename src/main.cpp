#include <mqueue.h>
#include <fcntl.h>              /* For definition of O_NONBLOCK */
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include "Gist.h"

const size_t frameSize = 512;
const size_t charsPerSample = 2; // 2 chars per 16bit PCM sample
const size_t samplesPerFrame = frameSize / charsPerSample;
const int sampleRate = 44100;

mqd_t write_mqd;
mqd_t read_mqd;
const char* queueName = "/samples\0";

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

void openMessageQueueForRead() {
  read_mqd = mq_open(queueName, O_RDONLY); // will block
  if (read_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << queueName << "' for read" << std::endl;
    exit(1);
  }
}

void writeFile() {
  std::ifstream file;
  file.open("recordings/Nightsong-signed16b-pcm.raw"); // little-endian

  mq_unlink(queueName);
  openMessageQueueForWrite();

  char *pcmInt16Frame = new char[frameSize];
  for(float t = 3.0; t < 3.5; t += 0.05) {
    std::cout << "Write from " << t << "s" << std::endl;
    file.seekg(t*44.1*1000.0*4.0);
    file.read(pcmInt16Frame, frameSize);
  //  std::cout << "First sample " << *reinterpret_cast<int16_t*>(pcmInt16Frame) << std::endl;

    if (mq_send(write_mqd, pcmInt16Frame, frameSize, 0) == -1) {
      std::cerr << "Can't send'" << std::endl;
      exit(1);
    }
  }

  std::cout << "sleep" << std::endl;
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10000ms);
  std::cout << "done sleep" << std::endl;
  mq_unlink(queueName);
  file.close();
}

void readMessages() {
  openMessageQueueForRead();

  while(true) {
    struct mq_attr attr;
    if (mq_getattr(read_mqd, &attr) == -1) {
      std::cerr << "Can't fetch attributes for mq '" << queueName << "'" << std::endl;
      exit(1);
    }
  //  std::cout << attr.mq_msgsize << std::endl;
  //  std::cout << attr.mq_maxmsg << std::endl;
  //  if (attr.mq_msgsize != frameSize) {
  //    std::cerr << "Message size != frameSize: " << attr.mq_msgsize << std::endl;
  //    exit(1);
  //  }
    char *receivedFrame = new char[attr.mq_msgsize];
    unsigned int prio;
    ssize_t sizeRead = mq_receive(read_mqd, receivedFrame, attr.mq_msgsize, &prio);
    if (sizeRead == -1) {
      std::cerr << "Can't receive" << std::endl;
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
      floatFrame[i / charsPerSample] = *(reinterpret_cast<int16_t*>(receivedFrame + i));
    }
    //float f = (int)*(receivedFrame) * 256 + (int)*(receivedFrame+1);
    //std::cout << "Big endian float " << f << std::endl;
    //float f2 = (int)*(receivedFrame+1) * 256 + (int)*(receivedFrame);
    //std::cout << "Little endian float " << f2 << std::endl;
    //std::cout << "First float " << int(floatFrame[0]) << std::endl;

    gist.processAudioFrame(floatFrame, samplesPerFrame);
    // float pitch = gist.pitch();
    // std::cout << pitch << std::endl;
    // float sd_hwr = gist.spectralDifferenceHWR();
    float csd = gist.complexSpectralDifference();
    if (csd > 200000) {
      std::cout << csd << std::endl;
    }
  }
}

// Execute with no args for a reader, else a writer
int main(int argc, char* argv[]) {
  if (argc > 1) {
    std::cout << "Start writer\n";
    writeFile();
  } else {
    std::cout << "Start reader\n";
    readMessages();
  }
}

