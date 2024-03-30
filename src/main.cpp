#include <mqueue.h>
#include <fcntl.h>              /* For definition of O_NONBLOCK */
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <memory>
#include <unordered_map>
#include <signal.h>
#define _BSD_SOURCE   /* To get definitions of NI_MAXHOST and NI_MAXSERV from <netdb.h> */
#include <netdb.h>
#define ADDRSTRLEN (NI_MAXHOST + NI_MAXSERV + 10)
#include <unistd.h>
#include "Gist.h"
#include <oscpp/client.hpp>

constexpr ssize_t MAX_MQ_MESSAGE_SIZE = 2048; // must be at least mq_msgsize

// The same code in jamulus, so whoever gets there first will create the queue
mqd_t read_mqd;
struct mq_attr read_attr;
const char* SAMPLES_QUEUE_NAME = "/samples"; // from Jamulus
void openMessageQueueForRead() {
  mode_t perms = S_IRUSR | S_IWUSR;
  read_attr.mq_maxmsg = 10;
  read_attr.mq_msgsize = MAX_MQ_MESSAGE_SIZE;
  read_mqd = mq_open(SAMPLES_QUEUE_NAME, O_RDONLY | O_CREAT, perms, &read_attr); // will block
  if (read_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << SAMPLES_QUEUE_NAME << "' for read" << std::endl;
    exit(1);
  }
  std::cout << "Opened mq for read" << std::endl;
  // Validate queue attributes
  if (mq_getattr(read_mqd, &read_attr) == -1) {
    std::cerr << "Can't fetch attributes for mq '" << SAMPLES_QUEUE_NAME << "'" << std::endl;
    return;
  }
  if (read_attr.mq_msgsize > MAX_MQ_MESSAGE_SIZE) {
    std::cerr << "mq_msgsize " << read_attr.mq_msgsize << " > MAX_MQ_MESSAGE_SIZE " << MAX_MQ_MESSAGE_SIZE << std::endl;
    return;
  }
}

// The same code in oscserver, so whoever gets there first will create the queue
mqd_t write_mqd;
struct mq_attr write_attr;
const char* OSC_QUEUE_NAME = "/osc";
void openMessageQueueForWrite() {
  mode_t perms = S_IRUSR | S_IWUSR;
  write_attr.mq_maxmsg = 10;
  write_attr.mq_msgsize = MAX_MQ_MESSAGE_SIZE;
  write_mqd = mq_open(OSC_QUEUE_NAME, O_WRONLY | O_CREAT, perms, &write_attr);
  if (write_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << OSC_QUEUE_NAME << "' for write" << std::endl;
    exit(1);
  }
  std::cout << "Opened mq for write" << std::endl;
  // Validate queue attributes
  if (mq_getattr(write_mqd, &write_attr) == -1) {
    std::cerr << "Can't fetch attributes for mq '" << OSC_QUEUE_NAME << "'" << std::endl;
    return;
  }
  if (write_attr.mq_msgsize > MAX_MQ_MESSAGE_SIZE) {
    std::cerr << "mq_msgsize " << write_attr.mq_msgsize << " > MAX_MQ_MESSAGE_SIZE " << MAX_MQ_MESSAGE_SIZE << std::endl;
    return;
  }
}

const size_t MAX_OSC_PACKET_SIZE = 512; // safe max is ethernet packet MTU 1500 (minus overhead gives max 1380) https://superuser.com/questions/1341012/practical-vs-theoretical-max-limit-of-tcp-packet-size
char oscBuffer[MAX_OSC_PACKET_SIZE];

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
  return packet.size();
}

const int SAMPLE_RATE = 48000; // for Gist: needs to match what Jamulus is sending

char receivedMeta[MAX_MQ_MESSAGE_SIZE];
char receivedFrame[MAX_MQ_MESSAGE_SIZE];
constexpr size_t MAX_MQ_FLOAT_FRAME_SIZE = MAX_MQ_MESSAGE_SIZE / sizeof(float);
float floatFrame[MAX_MQ_FLOAT_FRAME_SIZE];

// Copy this from Jamulus jamrecorder.cpp
static constexpr size_t MAX_OSC_FILEPATH_LENGTH = 64;
enum class META_TYPE { startSession=0, endSession, audioFrame };
struct startSessionMeta_t { int8_t metaType; char sessionDir[MAX_OSC_FILEPATH_LENGTH+1]; };
struct endSessionMeta_t  { int8_t metaType; };
struct audioMeta_t { int8_t metaType; int16_t channelId; uint64_t frameSequence; double offsetSeconds; char filename[MAX_OSC_FILEPATH_LENGTH+1]; };
// Jam-20240326-145726119/____-86_175_246_x_22141-0-1.wav

std::string oscDirectoryPrefix("/tmp/");
std::string oscDirectoryName; // populate on start of a session, clear on session end
std::unordered_map<int16_t, std::unique_ptr<std::ofstream>> oscFiles; // within a session, channelId -> ofstream ptr

void pipeMessages() {
  // open the MQ to read audio frames from Jamulus
  openMessageQueueForRead();
  // open the MQ to write OSC messages to oscserver
  openMessageQueueForWrite();

  // Flush the mq of old Jamulus messages to prepare for a new session
  unsigned int prio;
  read_attr.mq_flags = O_NONBLOCK;
  mq_setattr(read_mqd, &read_attr, NULL);
  ssize_t flushRead = 1;
  while(flushRead > -1) {
    flushRead = mq_receive(read_mqd, receivedMeta, read_attr.mq_msgsize, &prio);
  }
  read_attr.mq_flags = 0;
  mq_setattr(read_mqd, &read_attr, NULL);

  while(true) {

    ssize_t sizeRead = mq_receive(read_mqd, receivedFrame, read_attr.mq_msgsize, &prio);

    if (sizeRead == sizeof(startSessionMeta_t)) {
      startSessionMeta_t* meta = reinterpret_cast<startSessionMeta_t*>(receivedMeta);
      if (meta->metaType != static_cast<int8_t>(META_TYPE::startSession)) {
        std::cerr << "ignoring startSession meta, metaType " << meta->metaType << std::endl;
        continue;
      }
      oscDirectoryName = meta->sessionDir;
      std::string p = oscDirectoryPrefix + oscDirectoryName;
      std::filesystem::create_directory(p);
      // TODO: write metadata file
      oscFiles.clear(); // flushes, closes
      std::cout << "start session " <<  oscDirectoryName << std::endl;
      continue;
    }

    if (sizeRead == sizeof(endSessionMeta_t)) {
      endSessionMeta_t* meta = reinterpret_cast<endSessionMeta_t*>(receivedMeta);
      if (meta->metaType != static_cast<int8_t>(META_TYPE::endSession)) {
        std::cerr << "ignoring endSession meta, metaType " << meta->metaType << std::endl;
        continue;
      }
      oscFiles.clear(); // flushes, closes
      std::string p = oscDirectoryPrefix + oscDirectoryName;
      std::string cmd("aws s3 mv " + p + " s3://meyfroidt/osc/" + oscDirectoryName + " --recursive && rmdir " + p);
      std::system(cmd.c_str());
      std::cout << "end session" << oscDirectoryName << std::endl;
      oscDirectoryName = nullptr;
      continue;
    }

    if (oscDirectoryName == "") {
      std::cerr << "ignoring audio sent before session start" << std::endl;
      continue;
    }

    if (sizeRead != sizeof(audioMeta_t)) {
      std::cerr << "expected audio meta, but read unexpected message size " << sizeRead << std::endl;
      continue;
    }

    audioMeta_t* meta = reinterpret_cast<audioMeta_t*>(receivedMeta);
    if (meta->metaType != static_cast<int8_t>(META_TYPE::audioFrame)) {
      std::cerr << "ignoring audioFrame meta, metaType " << meta->metaType << std::endl;
      continue;
    }

    // the next message is an audio frame
    sizeRead = mq_receive(read_mqd, receivedFrame, read_attr.mq_msgsize, &prio);
    if (sizeRead < 200 || sizeRead % 2 == 1) {
      std::cerr << "ignoring audio frame with unexpected size " << sizeRead << std::endl;
      continue;
    }

    // samples from Jamulus are int16_t; Gist wants float32; so convert
    int sampleCount = sizeRead / sizeof(int16_t);
    for(ssize_t i = 0; i < sizeRead; i += sizeof(int16_t)) {
      floatFrame[i / sizeof(int16_t)] = static_cast<float>(*(reinterpret_cast<int16_t*>(receivedFrame + i))); // little-endian int16_t to float32
    }

    // Use Gist to analyse and then make an OSC packet
    Gist<float> gist(sampleCount, SAMPLE_RATE);
    gist.processAudioFrame(floatFrame, sampleCount);
    ssize_t bufferSize = makeOscPacket(meta->channelId, meta->frameSequence, gist);

    // Forward OSC to the oscserver
    if (mq_send(write_mqd, oscBuffer, bufferSize, 0) == -1) {
      std::cerr << "failed to send osc buffer" << std::endl;
    }

    // Create new file on first time we see a channel
    if (oscFiles.find(meta->channelId) == oscFiles.end()) {
      std::string filepath(oscDirectoryPrefix + oscDirectoryName + "/" + meta->filename + ".oscs");
      oscFiles[meta->channelId] = std::make_unique<std::ofstream>(filepath, std::ios::binary);
    }

    // TODO: find the last frame number written, write blanks (as special markers) so that
    // TODO: the file length is consistent throughout,
    oscFiles[meta->channelId]->write(oscBuffer, bufferSize);
  }
}

int main(int argc, char* argv[]) {
  // TODO: signal handler for ctrl-c

  std::cout << "Start OSC message pipeline\n";
  pipeMessages();

  std::cout << "exiting analyser" << std::endl;
}
