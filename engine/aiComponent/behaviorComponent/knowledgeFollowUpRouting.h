#pragma once

#include "json/json.h"
#include <string>

namespace Anki {
namespace Vector {

// Only used for an owned automatic KG capture, never an ordinary wake or the
// question sub-request of an already active KG behavior.
inline const char* PrepareKnowledgeFollowUpResult(Json::Value& json)
{
  const auto& intent = json["intent"];
  if (!intent.isString()) { return "malformed_result"; }
  const auto name = intent.asString();
  if (name == "intent_system_noaudio") { return "silence"; }
  if (name != "intent_knowledge_response_extend" &&
      name != "intent_knowledge_response_extend_bypass") {
    return "non_eligible_result";
  }
  const auto& params = json["params"];
  if (!params.isObject() || !params["query_text"].isString() ||
      !params["answer"].isString()) {
    return "malformed_result";
  }
  const auto query = params["query_text"].asString();
  const auto first = query.find_first_not_of(" \t\r\n");
  // KG's proto3 QueryText can be absent even for a valid spoken answer.
  // An unavailable transcript is not a silence signal; keep explicit noaudio,
  // empty-answer and recognized stop handling without inventing user text.
  auto command = first == std::string::npos ? std::string{} :
    query.substr(first, query.find_last_not_of(" \t\r\n") - first + 1);
  while (!command.empty() && std::string(".!?").find(command.back()) != std::string::npos) {
    command.pop_back();
  }
  for (auto& c : command) {
    if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; }
  }
  // Exact user transcripts only, not words found in a generated answer.
  if (command == "stop" || command == "cancel" || command == "stop listening" ||
      command == "end conversation") {
    return "spoken_stop";
  }
  if (params["answer"].asString().find_first_not_of(" \t\r\n") == std::string::npos) {
    return "empty_answer";
  }
  // Preserve every response/audio field; only change the dispatch tag so the
  // responder consumes this answer rather than asking another question.
  json["intent"] = "intent_knowledge_response_extend_bypass";
  return nullptr;
}

}
}
