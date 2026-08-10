#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * SimilarBooksActivity
 *
 * Takes the most recently opened book (from RecentBooksStore) and looks up
 * similar titles via the Open Library search API, over SecureHttpClient.
 *
 * Mirrors OpdsBookBrowserActivity's state-machine shape (CHECK_WIFI ->
 * WIFI_SELECTION -> LOADING -> RESULTS/ERROR) and RecentBooksActivity's list
 * rendering via GUI.drawList.
 */
class SimilarBooksActivity final : public Activity {
 public:
  enum class State { NO_SOURCE_BOOK, CHECK_WIFI, WIFI_SELECTION, LOADING, RESULTS, ERROR };

  struct SimilarBook {
    std::string title;
    std::string author;
  };

  explicit SimilarBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SimilarBooks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::CHECK_WIFI;

  // Source book pulled from RecentBooksStore on entry (most recent).
  std::string sourceTitle;
  std::string sourceAuthor;

  std::vector<SimilarBook> results;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);

  // Runs the blocking HTTPS request + JSON parse. Updates state/results/errorMessage.
  void fetchSimilarBooks();

  bool preventAutoSleep() override { return true; }
};
