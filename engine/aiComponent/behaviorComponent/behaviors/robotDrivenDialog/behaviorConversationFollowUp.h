#pragma once

#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"

namespace Anki {
namespace Vector {
class BehaviorConversationFollowUp : public ICozmoBehavior
{
  friend class BehaviorFactory;
  explicit BehaviorConversationFollowUp(const Json::Value& config);
  bool WantsToBeActivatedBehavior() const override;
  void GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const override;
  void GetBehaviorJsonKeys(std::set<const char*>&) const override {}
  void OnBehaviorActivated() override;
  void OnBehaviorDeactivated() override;
  void BehaviorUpdate() override;
  uint32_t _streamId = 0;
  bool _listening = false;
  bool _captureClosed = false;
  bool _gettingOut = false;
};
}
}
