// FrameWave v3.3: the stream overlay (chat, viewers, donations, heart rate).
//
// Everything here is cosmetic: it reads the real level percentage from PlayLayer and shows a
// Twitch-style setup on top of the game. Nothing is sent anywhere and nothing costs money.
//
// The overlay itself never prints the words "fake" or "simulated" (that's a hard requirement):
// it looks like a normal stream layout. Only this menu uses those words.
#pragma once

#include <Geode/Geode.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace streamui {

    struct Bot {
        std::string name;
        int color = 0;              // index into the palette (see colorOf)
        int badge = 0;              // 0 none, 1 mod, 2 vip, 3 sub, 4 founder
        float freq = 1.f;           // how often this bot talks, relative to the others
        int hype = 50;              // 0 calm .. 100 all caps
        int style = 0;              // 0 normal, 1 caps, 2 stretch, 3 emotes, 4 one-word
        bool custom = false;
        std::vector<std::string> lines;   // custom bots can bring their own messages
    };

    // ---- lifecycle ----
    void load();                                        // first call: builds the bot list
    void tick(cocos2d::CCNode* layer, float dt);        // once per frame, from PlayLayer
    void refresh();                                     // settings changed in the menu
    void resetOverlay();
    void leaveLevel();     // the level node is going away: drop the overlay tree
    void clearChat();
    void previewMessage();                              // "send a test message"
    void testDonation();
    void heartSpike();

    // ---- level events (called from the PlayLayer hooks) ----
    void onDeath(float pct);
    void onComplete();
    void onRestart();
    void onProgress(float pct);                         // per frame, cheap
    void setLevelName(std::string name);                // so chat and donations can mention it

    // ---- readouts for the menu ----
    bool chatOn();
    bool viewersOn();
    bool giftsOn();
    bool heartOn();
    int messageCount();        // lines in the chat database
    int giftMessageCount();    // lines in the donation message pool
    int activeBotCount();
    int totalBotCount();
    int currentBpm();
    int currentViewers();
    int giftQueueSize();
    int ttsQueueSize();

    // ---- editing ----
    std::vector<Bot> const& bots();                     // active participants first
    void upsertBot(int index, Bot const& bot);          // index < 0 -> append
    void eraseBot(int index);
    std::vector<std::string> customGiftMessages();
    void addGiftMessage(std::string const& text);
    void eraseGiftMessage(int index);

    // ---- text to speech ----
    bool ttsAvailable();
    int voiceCount();
    std::string voiceName(int index);
    void speakTest();
    void ttsClear();

    // ---- small helpers shared with the menu ----
    std::vector<Bot> const& customBots();               // the user's own bots (editable)
    bool validName(std::string const& name);            // same screen as the generated names
    void drawBadgePreview(cocos2d::CCDrawNode* d, int kind, cocos2d::CCPoint at, float px);

    // ---- moving / resizing the panel by touch (the overlay is on top of the level) ----
    bool touchDown(cocos2d::CCPoint const& world);   // true when the overlay took the touch
    void touchMove(cocos2d::CCPoint const& world);
    void touchUp();
    bool dragging();
    void setChipAction(std::function<void()> fn);    // the small "menu" chip
    void setChipVisible(bool on);
    cocos2d::ccColor3B colorOf(int index);
    int colorCount();
    char const* colorName(int index);
    char const* badgeName(int index);
    int badgeCount();

    // set by the menu when something changed and the page should rebuild
    extern bool dirty;
    // filled by the first load(): the sizes of the two text databases, for the menu
    extern int publishedChat;
    extern int publishedGifts;
}
