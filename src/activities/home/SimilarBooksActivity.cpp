#include "SimilarBooksActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// TODO(i18n): these strings are plain literals rather than tr(STR_...) — wire
// them into I18nKeys.h / the translation tables before upstreaming. Fine for
// a personal build in the meantime.
namespace {
constexpr const char* TXT_TITLE = "Similar Books";
constexpr const char* TXT_CHECKING_WIFI = "Checking WiFi...";
constexpr const char* TXT_LOADING = "Finding similar books...";
constexpr const char* TXT_NO_SOURCE = "Read a book first so I have something to compare against.";
constexpr const char* TXT_WIFI_FAILED = "WiFi connection failed.";
constexpr const char* TXT_FETCH_FAILED = "Could not reach Open Library. Try again later.";
constexpr const char* TXT_PARSE_FAILED = "Unexpected response from Open Library.";
constexpr const char* TXT_NO_RESULTS = "No similar books found.";
constexpr const char* TXT_LOW_MEMORY = "Not enough free memory for a network request right now.";
constexpr const char* TXT_BASED_ON_PREFIX = "Based on: ";

// Same heap-gate thresholds KOReaderSyncClient uses before a TLS handshake:
// see lib/KOReaderSync/KOReaderSyncClient.cpp for the measurement rationale.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

constexpr int MAX_RESULTS = 15;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("SIMBOOKS", "Insufficient heap for TLS handshake: %u free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

// Percent-encodes a query-param value (RFC 3986 unreserved set kept literal).
std::string urlEncode(const std::string& value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += HEX_DIGITS[(c >> 4) & 0xF];
      out += HEX_DIGITS[c & 0xF];
    }
  }
  return out;
}
}  // namespace

void SimilarBooksActivity::onEnter() {
  Activity::onEnter();

  results.clear();
  selectorIndex = 0;
  errorMessage.clear();

  // Most recent entry in RecentBooksStore = last book opened (list is kept
  // most-recent-first; see RecentBooksStore::addBook).
  const auto& recent = RECENT_BOOKS.getBooks();
  if (recent.empty() || recent.front().title.empty()) {
    state = State::NO_SOURCE_BOOK;
    requestUpdate();
    return;
  }

  sourceTitle = recent.front().title;
  sourceAuthor = recent.front().author;

  state = State::CHECK_WIFI;
  statusMessage = TXT_CHECKING_WIFI;
  requestUpdate();

  checkAndConnectWifi();
}

void SimilarBooksActivity::onExit() {
  Activity::onExit();
  results.clear();

  // Mirror OpdsBookBrowserActivity: leave WiFi teardown to a silent restart
  // isn't needed here since we never keep a long-lived connection open past
  // fetchSimilarBooks() — SecureHttpClient::end() (via destructor) closes it.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
  }
}

void SimilarBooksActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::LOADING;
    statusMessage = TXT_LOADING;
    requestUpdate();
    fetchSimilarBooks();
    return;
  }
  launchWifiSelection();
}

void SimilarBooksActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void SimilarBooksActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = State::LOADING;
    statusMessage = TXT_LOADING;
    requestUpdate(true);
    fetchSimilarBooks();
  } else {
    state = State::ERROR;
    errorMessage = TXT_WIFI_FAILED;
    requestUpdate();
  }
}

void SimilarBooksActivity::fetchSimilarBooks() {
  if (insufficientHeap()) {
    state = State::ERROR;
    errorMessage = TXT_LOW_MEMORY;
    requestUpdate();
    return;
  }

  // Prefer searching by author (broader, catches translations/other works);
  // fall back to title if the source book has no author on record.
  const std::string& query = !sourceAuthor.empty() ? sourceAuthor : sourceTitle;
  const std::string field = !sourceAuthor.empty() ? "author" : "q";

  const std::string url = "https://openlibrary.org/search.json?" + field + "=" + urlEncode(query) +
                          "&limit=" + std::to_string(MAX_RESULTS) + "&fields=title,author_name";

  LOG_DBG("SIMBOOKS", "Fetching: %s", url.c_str());

  freeink::SecureHttpClient http;
  http.setInsecure();  // matches KOReaderSyncClient — no CA bundle wired up for wolfSSL yet
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("CrossPoint-SimilarBooks/1.0");

  if (!http.begin(url)) {
    state = State::ERROR;
    errorMessage = TXT_FETCH_FAILED;
    requestUpdate();
    return;
  }
  http.addHeader("Accept", "application/json");

  const int status = http.GET();
  if (status != 200) {
    LOG_ERR("SIMBOOKS", "Open Library request failed, status=%d", status);
    state = State::ERROR;
    errorMessage = TXT_FETCH_FAILED;
    requestUpdate();
    return;
  }

  // Filtered deserialization: Open Library's raw docs are large (dozens of
  // fields per entry). Asking ArduinoJson to only materialize title/
  // author_name keeps the parse heap-cheap on the C3.
  JsonDocument filter;
  filter["docs"][0]["title"] = true;
  filter["docs"][0]["author_name"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    LOG_ERR("SIMBOOKS", "JSON parse failed: %s", err.c_str());
    state = State::ERROR;
    errorMessage = TXT_PARSE_FAILED;
    requestUpdate();
    return;
  }

  results.clear();
  for (JsonObject doc_entry : doc["docs"].as<JsonArray>()) {
    const char* title = doc_entry["title"] | "";
    if (!title || !title[0]) continue;

    // Skip the source book itself if Open Library echoes it back.
    if (sourceTitle == title) continue;

    std::string author;
    if (JsonArray authors = doc_entry["author_name"]; !authors.isNull() && authors.size() > 0) {
      author = authors[0].as<const char*>();
    }

    results.push_back(SimilarBook{title, author});
    if (static_cast<int>(results.size()) >= MAX_RESULTS) break;
  }

  state = State::RESULTS;
  requestUpdate(true);
}

void SimilarBooksActivity::loop() {
  if (state == State::WIFI_SELECTION) return;

  if (state == State::NO_SOURCE_BOOK || state == State::LOADING || state == State::CHECK_WIFI) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onGoHome();
    }
    return;
  }

  // state == RESULTS
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int count = static_cast<int>(results.size());
  if (count == 0) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  int touchSel = selectorIndex;
  const auto listTouch = handleListTouch(touchSel, count, contentTop, contentHeight, true);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = touchSel;
    requestUpdate();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, count, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, count, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, count] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
  });
}

void SimilarBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, TXT_TITLE,
                (state == State::RESULTS && !sourceTitle.empty())
                    ? (std::string(TXT_BASED_ON_PREFIX) + sourceTitle).c_str()
                    : nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  switch (state) {
    case State::NO_SOURCE_BOOK:
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, TXT_NO_SOURCE);
      break;
    case State::CHECK_WIFI:
    case State::LOADING:
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, statusMessage.c_str());
      break;
    case State::ERROR:
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, errorMessage.c_str());
      break;
    case State::RESULTS:
      if (results.empty()) {
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, TXT_NO_RESULTS);
      } else {
        GUI.drawList(
            renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(results.size()), selectorIndex,
            [this](int index) { return results[index].title; },
            [this](int index) { return results[index].author; }, [](int) { return UIIcon::Book; });
      }
      break;
    case State::WIFI_SELECTION:
      break;
  }

  GUI.drawButtonHints(renderer, "Back", "", "Prev", "Next");

  renderer.displayBuffer();
}
