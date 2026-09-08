#include "util/helpers/includeGTest.h"
#include "engine/aiComponent/behaviorComponent/conversationSessionState.h"
#include <limits>

using Anki::Vector::ConversationSessionState;
using State = ConversationSessionState::State;
using Outcome = ConversationSessionState::Outcome;

namespace {
void Answer(ConversationSessionState& session, size_t activation, double now)
{
  session.ResponseFinished(session.Token(), Outcome::Succeeded);
  session.Deactivated(activation, now);
}

void Prepare(ConversationSessionState& session)
{
  session.Claimed(1, true, true, 0);
  Answer(session, 1, 25);
  session.Quiesced(25);
}
}

TEST(ConversationSession, DefaultOnButNeverListensAtBoot)
{
  ConversationSessionState session;
  EXPECT_TRUE(session.config.enabled);
  EXPECT_FALSE(session.Active());
  EXPECT_FALSE(session.Ready(100));
}

TEST(ConversationSession, OmitCelebrationOnlyForCurrentAdmissibleResponse)
{
  ConversationSessionState session;
  EXPECT_FALSE(session.CanContinueResponse(session.Token(), 0));
  session.Claimed(1, true, true, 0);
  const auto token = session.Token();
  EXPECT_TRUE(session.CanContinueResponse(token, 25));
  EXPECT_FALSE(session.CanContinueResponse(token + 1, 25));
  EXPECT_FALSE(session.CanContinueResponse(token, 120));
  session.config.maxTurns = 1;
  EXPECT_FALSE(session.CanContinueResponse(token, 25));
  session.config.maxTurns = 5;
  session.config.enabled = false;
  EXPECT_FALSE(session.CanContinueResponse(token, 25));
  session.config.enabled = true;
  session.ResponseFinished(token, Outcome::Succeeded);
  EXPECT_FALSE(session.Ready(25));
  session.Deactivated(1, 25);
  EXPECT_FALSE(session.CanContinueResponse(token, 25));
  session.Quiesced(25);
  EXPECT_FALSE(session.Ready(25.249));
  EXPECT_TRUE(session.Ready(25.25));
}

TEST(ConversationSession, DisabledPreservesSingleTurn)
{
  ConversationSessionState session;
  session.config.enabled = false;
  Prepare(session);
  EXPECT_FALSE(session.Active());
  EXPECT_FALSE(session.Open(26));
}

TEST(ConversationSession, RequiresEligibleVoiceActivation)
{
  ConversationSessionState session;
  session.Claimed(1, false, true, 0);
  EXPECT_FALSE(session.Active());
  session.Claimed(2, true, false, 0);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, RequiresExplicitPlaybackAndOwnerDeactivation)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  session.ResponseFinished(session.Token(), Outcome::Succeeded);
  EXPECT_FALSE(session.Ready(30));
  session.Deactivated(2, 30);
  EXPECT_EQ(State::Responding, session.GetState());
  session.Deactivated(1, 30);
  EXPECT_EQ(State::Quiescing, session.GetState());
}

TEST(ConversationSession, DeactivationAloneCannotAuthorizeListening)
{
  for (const auto outcome : {Outcome::Failed, Outcome::Cancelled}) {
    ConversationSessionState session;
    session.Claimed(1, true, true, 0);
    session.ResponseFinished(session.Token(), outcome);
    session.Deactivated(1, 25);
    EXPECT_FALSE(session.Active());
  }
}

TEST(ConversationSession, WaitsForCaptureQuiescenceAndFullSettle)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  Answer(session, 1, 25);
  EXPECT_FALSE(session.Ready(25.5));
  session.Quiesced(25.5);
  EXPECT_FALSE(session.Ready(25.749));
  EXPECT_TRUE(session.Ready(25.75));
}

TEST(ConversationSession, OpensExactlyOnceAfterLongAnswer)
{
  ConversationSessionState session;
  Prepare(session);
  ASSERT_TRUE(session.Open(25.25));
  EXPECT_FALSE(session.Open(25.25));
  EXPECT_EQ(2u, session.Turn());
  session.CaptureOpened(25.5);
  session.Update(90.49, true);
  EXPECT_TRUE(session.Active());
  session.Update(90.5, true);
  EXPECT_FALSE(session.Active());
  EXPECT_STREQ("result_timeout", session.EndReason());
}

TEST(ConversationSession, QuiescenceAndActivationHaveNoRetries)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  Answer(session, 1, 25);
  session.Update(27, true);
  EXPECT_FALSE(session.Active());
  EXPECT_FALSE(session.Open(30));
  Prepare(session);
  session.Update(27.25, true);
  EXPECT_FALSE(session.Active());
  EXPECT_FALSE(session.Open(28));
}

TEST(ConversationSession, OpeningWatchdogIsFiveSeconds)
{
  ConversationSessionState session;
  Prepare(session);
  ASSERT_TRUE(session.Open(25.25));
  session.Update(30.25, true);
  EXPECT_STREQ("opening_timeout", session.EndReason());
  session.CaptureOpened(31);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, StaleCompletionCannotReviveCancelledGeneration)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  const auto token = session.Token();
  session.End("cancelled");
  session.Claimed(2, true, true, 10);
  session.ResponseFinished(token, Outcome::Succeeded);
  session.Deactivated(2, 25);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, SuccessfulFallbackProducesOneContinuation)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  // Renderer adapters emit only the final response outcome after any fallback.
  Answer(session, 1, 25);
  session.ResponseFinished(session.Token(), Outcome::Succeeded);
  session.Deactivated(1, 25);
  session.Quiesced(25);
  ASSERT_TRUE(session.Open(25.25));
  session.Deactivated(1, 26);
  EXPECT_EQ(State::Opening, session.GetState());
}

TEST(ConversationSession, FiveTurnLimitIncludesInitialQuestion)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  for (size_t activation = 1; activation <= 5; ++activation) {
    const double now = activation * 10;
    Answer(session, activation, now);
    if (activation == 5) {
      EXPECT_FALSE(session.Active());
    } else {
      session.Quiesced(now);
      ASSERT_TRUE(session.Open(now + .25));
      session.CaptureOpened(now + .5);
      session.Result(true, now + 1);
      session.Claimed(activation + 1, true, true, now + 1);
    }
  }
  EXPECT_EQ(5u, session.Turn());
}

TEST(ConversationSession, AdmissionDeadlineDoesNotInterruptAdmittedAnswer)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  session.Update(121, true);
  EXPECT_EQ(State::Responding, session.GetState());
  Answer(session, 1, 150);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, DeadlineDoesNotInterruptAdmittedListening)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  Answer(session, 1, 119);
  session.Quiesced(119);
  ASSERT_TRUE(session.Open(119.25));
  session.CaptureOpened(119.5);
  session.Update(121, true);
  EXPECT_EQ(State::Listening, session.GetState());
  session.Result(true, 122);
  session.Claimed(2, true, true, 122);
  Answer(session, 2, 150);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, SilenceErrorsAndNonEligibleCommandsEndOnce)
{
  for (const auto* reason : {"silence", "unmatched", "transport_error", "cancelled"}) {
    ConversationSessionState session;
    Prepare(session);
    ASSERT_TRUE(session.Open(25.25));
    const auto token = session.Token();
    session.End(reason);
    const auto invalidated = session.Token();
    EXPECT_GT(invalidated, token);
    session.End(reason);
    EXPECT_EQ(invalidated, session.Token());
    session.Result(true, 26);
    EXPECT_FALSE(session.Active());
  }
  ConversationSessionState session;
  Prepare(session);
  ASSERT_TRUE(session.Open(25.25));
  session.Result(false, 26);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, DisableOrSafetyDuringSettleRevokesRequest)
{
  ConversationSessionState disabled, unsafe;
  Prepare(disabled);
  Prepare(unsafe);
  disabled.config.enabled = false;
  disabled.Update(25.1, true);
  unsafe.Update(25.1, false);
  EXPECT_FALSE(disabled.Open(25.3));
  EXPECT_FALSE(unsafe.Open(25.3));
}

TEST(ConversationSession, LateResultCannotEscapeOpeningWatchdog)
{
  ConversationSessionState session;
  Prepare(session);
  ASSERT_TRUE(session.Open(25.25));
  EXPECT_FALSE(session.CanReceiveResult(30.25));
  session.Result(true, 30.25);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, PendingResultMustBeClaimedBeforeDeadline)
{
  ConversationSessionState session;
  Prepare(session);
  ASSERT_TRUE(session.Open(25.25));
  session.Result(true, 26);
  session.Update(28, true);
  EXPECT_FALSE(session.Active());
  EXPECT_FALSE(session.Open(29));
}

TEST(ConversationSession, CancelAfterPlaybackBeforeDeactivationRevokesSuccess)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  session.ResponseFinished(session.Token(), Outcome::Succeeded);
  session.ResponseFinished(session.Token(), Outcome::Cancelled);
  session.Deactivated(1, 25);
  EXPECT_FALSE(session.Active());
}

TEST(ConversationSession, RejectsInvalidAdmissionConfiguration)
{
  ConversationSessionState::Config config;
  EXPECT_TRUE(config.IsValid());
  config.enabled = false;
  EXPECT_TRUE(config.IsValid());
  config.maxTurns = 0;
  EXPECT_FALSE(config.IsValid());
  config.maxTurns = 21;
  EXPECT_FALSE(config.IsValid());
  config.maxTurns = 5;
  for (double value : {-1.0, 0.0, 601.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    config.sessionTimeout_sec = value;
    EXPECT_FALSE(config.IsValid());
  }
  config.sessionTimeout_sec = 120;
  for (double value : {-1.0, 2001.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    config.followUpSettleTime_ms = value;
    EXPECT_FALSE(config.IsValid());
  }
}
