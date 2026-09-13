#pragma once
#include <Arduino.h>
#include <vector>

// Question pushed from the Mac by the `claude-stick` CLI.
enum AskStatus { ASK_IDLE, ASK_PENDING, ASK_ANSWERED, ASK_DISMISSED, ASK_TIMEOUT };

class ClaudeLink {
public:
  void begin(const String& hostname);
  void loop();

  // --- current question -------------------------------------------------
  String                id;
  String                title;
  String                question;
  std::vector<String>   options;
  int                   selected  = -1;
  AskStatus             status    = ASK_IDLE;
  uint32_t              deadlineMs = 0;

  bool pending() const { return status == ASK_PENDING; }
  void answer(int index);
  void dismiss();

  // --- transient banner pushed by `claude-stick notify` -----------------
  String   notifyText;
  String   notifyLevel;
  uint32_t notifyUntilMs = 0;
  bool notifying() const { return notifyUntilMs && millis() < notifyUntilMs; }

  // Usage limits, pushed from the Mac by `claude-stick usage`.
  // Percentages are what has been SPENT, exactly as Claude Code reports it.
  // -1 = unknown.
  int      usageFiveHourUsed = -1;
  int      usageSevenDayUsed = -1;
  uint32_t usageFiveHourReset = 0;   // unix seconds, 0 = unknown
  uint32_t usageSevenDayReset = 0;
  uint64_t usageTokensToday   = 0;
  bool     usageStale         = true;
  uint32_t usageReceivedMs    = 0;
  bool     hasUsage() const { return usageReceivedMs != 0; }

  // Set whenever incoming traffic should pull the UI to the front.
  bool     wake = false;
  uint32_t asksServed = 0;

  String hostname;
  const char* firmware = "?";
  const char* build = "?";
  uint32_t reconnects = 0;
  void restartMdns();

  // Over-the-air firmware update state
  bool     otaDenied = false;
  uint32_t otaBytes  = 0;
  uint32_t otaTotal  = 0;

private:
  bool authorised();
  void routes();
  const char* statusName() const;
};

extern ClaudeLink claudeLink;
