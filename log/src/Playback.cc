/*
 * Copyright (C) 2017 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <ignition/common/Console.hh>
#include <ignition/transport/Node.hh>

#include "ignition/transport/log/Playback.hh"
#include "ignition/transport/log/Log.hh"
#include "src/raii-sqlite3.hh"
#include "build_config.hh"


using namespace ignition::transport;
using namespace ignition::transport::log;

/// \brief Private implementation
class ignition::transport::log::PlaybackPrivate
{
  /// \brief Create a publisher of a given topic name and type
  public: bool CreatePublisher(
              const std::string &_topic, const std::string &_type);

  /// \brief True if the thread should stop
  public: bool stop = false;

  /// \brief thread running playback
  public: std::thread playbackThread;

  /// \brief log file to play from
  public: Log logFile;

  /// \brief mutex for thread safety with log file
  public: std::mutex logFileMutex;

  /// \brief node used to create publishers
  public: ignition::transport::Node node;

  /// \brief topics that are being played back
  public: std::unordered_set<std::string> topicNames;

  /// \brief Map whose key is a topic name and value is another map whose
  /// key is a message type name and value is a publisher
  public: std::unordered_map<std::string,
          std::unordered_map<std::string,
            ignition::transport::Node::Publisher>> publishers;
};

//////////////////////////////////////////////////
bool PlaybackPrivate::CreatePublisher(
    const std::string &_topic, const std::string &_type)
{
  auto firstMapIter = this->publishers.find(_topic);
  if (firstMapIter == this->publishers.end())
  {
    // Create a map for the message topic
    this->publishers[_topic] = std::unordered_map<std::string,
      ignition::transport::Node::Publisher>();
    firstMapIter = publishers.find(_topic);
  }

  auto secondMapIter = firstMapIter->second.find(_type);
  if (secondMapIter != firstMapIter->second.end())
  {
    return false;
  }

  // Create a publisher for the topic and type combo
  firstMapIter->second[_type] = this->node.Advertise(
      _topic, _type);
  igndbg << "Creating publisher for " << _topic << " " << _type << "\n";
  return true;
}

//////////////////////////////////////////////////
Playback::Playback(const std::string &_file)
  : dataPtr(new PlaybackPrivate)
{
  if (!this->dataPtr->logFile.Open(_file, std::ios_base::in))
  {
    ignerr << "Failed to open file [" << _file << "]\n";
  }
  else
  {
    igndbg << "Playback opened file [" << _file << "]\n";
  }
}

//////////////////////////////////////////////////
Playback::Playback(Playback &&_other)  // NOLINT
  : dataPtr(std::move(_other.dataPtr))
{
}

//////////////////////////////////////////////////
Playback::~Playback()
{
  this->Stop();
}

//////////////////////////////////////////////////
PlaybackError Playback::Start()
{
  std::lock_guard<std::mutex> lock(this->dataPtr->logFileMutex);
  if (!this->dataPtr->logFile.Valid())
  {
    ignerr << "Failed to open log file\n";
    return PlaybackError::FAILED_TO_OPEN;
  }

  auto iter = this->dataPtr->logFile.AllMessages();
  if (MsgIter() == iter)
  {
    ignwarn << "There are no messages to play\n";
    return PlaybackError::NO_MESSAGES;
  }

  ignmsg << "Started playing\n";

  this->dataPtr->playbackThread = std::thread(
    [this, iter{std::move(iter)}] () mutable
    {
      bool publishedFirstMessage = false;
      // Map of topic names to a map of message types to publisher
      std::unordered_map<std::string,
        std::unordered_map<std::string,
        ignition::transport::Node::Publisher>> publishers;

      ignition::common::Time nextTime;
      ignition::common::Time lastMessageTime;

      std::lock_guard<std::mutex> playbackLock(this->dataPtr->logFileMutex);
      while (!this->dataPtr->stop && MsgIter() != iter)
      {
        //Publish the first message right away, all others delay
        if (publishedFirstMessage)
        {
          // TODO use steady_clock to track the time spent publishing the last message
          // and alter the delay accordingly
          ignition::common::Time delta = nextTime - lastMessageTime;

          std::this_thread::sleep_for(std::chrono::nanoseconds(
                delta.sec * 1000000000 + delta.nsec));
        }
        else
        {
          publishedFirstMessage = true;
        }

        // Do some stuff that won't be needed with a raw publisher
        // of ignition messages this executable is linked with
        const google::protobuf::Descriptor* descriptor =
          google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(iter->Type());
        if (!descriptor)
        {
          ignerr << "This message cannot be published until ignition transport has a raw_bytes publisher(1)\n";
          continue;
        }
        const google::protobuf::Message* prototype = NULL;
        prototype = google::protobuf::MessageFactory::generated_factory()->GetPrototype(descriptor);

        if (!prototype)
        {
          ignerr << "This message cannot be published until ignition transport has a raw_bytes publisher(2)\n";
          continue;
        }

        // Actually publish the message
        google::protobuf::Message* payload = prototype->New();
        payload->ParseFromString(iter->Data());
        igndbg << "publishing\n";
        this->dataPtr->publishers[iter->Topic()][iter->Type()].Publish(*payload);
        lastMessageTime = nextTime;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++iter;
      }
    });

  return PlaybackError::NO_ERROR;
}

//////////////////////////////////////////////////
PlaybackError Playback::Stop()
{
  if (!this->dataPtr->logFile.Valid())
  {
    ignerr << "Failed to open log file\n";
    return PlaybackError::FAILED_TO_OPEN;
  }
  this->dataPtr->stop = true;
  this->dataPtr->playbackThread.join();
  return PlaybackError::NO_ERROR;
}


//////////////////////////////////////////////////
PlaybackError Playback::AddTopic(const std::string &_topic)
{
  if (!this->dataPtr->logFile.Valid())
  {
    ignerr << "Failed to open log file\n";
    return PlaybackError::FAILED_TO_OPEN;
  }

  std::vector<Log::NameTypePair> allTopics = this->dataPtr->logFile.AllTopics();
  for (const Log::NameTypePair &topicType : allTopics)
  {
    if (topicType.first == _topic)
    {
      igndbg << "Playing back [" << _topic << "]\n";
      // Save the topic name for the query in Start
      this->dataPtr->topicNames.insert(_topic);
      if (!this->dataPtr->CreatePublisher(topicType.first, topicType.second))
      {
        return PlaybackError::FAILED_TO_ADVERTISE;
      }
      return PlaybackError::NO_ERROR;
    }
  }

  ignerr << "Topic [" << _topic << "] is not in the log\n";
  return PlaybackError::NO_SUCH_TOPIC;
}

//////////////////////////////////////////////////
int Playback::AddTopic(const std::regex &_topic)
{
  if (!this->dataPtr->logFile.Valid())
  {
    ignerr << "Failed to open log file\n";
    return static_cast<int>(PlaybackError::FAILED_TO_OPEN);
  }
  // for all topics in the log
  //  if it matches the regex
  //    this->AddTopic(topicName)
  int numPublishers = 0;
  std::vector<Log::NameTypePair> allTopics = this->dataPtr->logFile.AllTopics();
  for (const Log::NameTypePair &topicType : allTopics)
  {
    if (std::regex_match(topicType.first, _topic))
    {
      if (this->dataPtr->CreatePublisher(topicType.first, topicType.second))
      {
        ++numPublishers;
      }
      else
      {
        return static_cast<int>(PlaybackError::FAILED_TO_ADVERTISE);
      }
    }
    else
    {
      igndbg << "Not playing back " << topicType.first << " " <<
        topicType.second << "\n";
    }
  }
  return numPublishers;
}
