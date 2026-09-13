#pragma once
#include <Arduino.h>
#include <vector>

struct Entity {
  String  id;          // light.biurko
  String  domain;      // light
  String  name;        // Biurko
  String  area;        // Sypialnia ("" when unassigned)
  String  state;       // on / off / playing / unavailable ...
  int     brightness;  // 0..255, -1 when not applicable

  bool isOn() const { return state == "on" || state == "playing" || state == "open" ||
                             state == "unlocked" || state == "cleaning" || state == "heat" ||
                             state == "cool" || state == "home"; }
  bool isUnavailable() const { return state == "unavailable" || state == "unknown"; }
};

// A single thing the user can do to an entity, rendered as a row in the detail view.
struct Action {
  const char* label;
  const char* domain;   // service domain ("" -> use entity domain)
  const char* service;
  int         arg;      // brightness step / volume step / temperature delta, 0 when unused
};

class HaClient {
public:
  std::vector<Entity> entities;

  bool     online = false;
  String   lastError;
  uint32_t lastRefreshMs = 0;

  bool refresh();                                   // pull every controllable entity
  bool callService(const String& domain, const String& service,
                   const String& entityId, const String& extraJson = "");
  bool runAction(const Entity& e, const Action& a);
  bool refreshOne(const String& entityId);          // update a single entity in place

  Entity*  find(const String& id);
  size_t   countDomain(const char* domain) const;
  std::vector<String> areas() const;

  // Actions offered for an entity's domain; writes into `out`, returns count.
  static int actionsFor(const Entity& e, Action* out, int maxOut);
  static const char* domainLabel(const char* domain);

private:
  bool request(const char* method, const String& path, const String& body,
               String& response, int& code);
};

extern HaClient ha;
