#pragma once

#include "engine/aiComponent/behaviorComponent/conversationSessionState.h"
#include "engine/aiComponent/behaviorComponent/behaviorComponents_fwd.h"
#include "util/entityComponent/iDependencyManagedComponent.h"

namespace Json { class Value; }
namespace Anki {
namespace Vector {
class UserIntentComponent;

class ConversationSessionComponent : public IDependencyManagedComponent<BCComponentID>
{
public:
  ConversationSessionComponent();
  void GetInitDependencies(BCCompIDSet& deps) const override;
  void GetUpdateDependencies(BCCompIDSet& deps) const override;
  void InitDependent(Robot* robot, const BCCompMap& comps) override;
  void UpdateDependent(const BCCompMap& comps) override;
  void Configure(const Json::Value& config);
  ConversationSessionState& Policy() { return _policy; }
  const ConversationSessionState& Policy() const { return _policy; }
  bool WantsFollowUp() const;
  uint32_t OpenFollowUp();
  void CancelFollowUp(uint32_t streamId);
  bool IsSafe() const;
  bool IsEnabled() const;

private:
  Robot* _robot = nullptr;
  UserIntentComponent* _uic = nullptr;
  const BCCompMap* _comps = nullptr;
  ConversationSessionState _policy;
  uint32_t _ownedStream = 0;
  bool _stopRequested = false;
  bool _hadSession = false;
};
}
}
