#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"
#include "ImGuiFileDialog.h"
#include "BotAttempt.h"

#include "version.h"
#include "Attempt.h"
#include "KickoffSpawns.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

using namespace std;

enum class SpeedFlipTrainerMode
{
	Replay,
	Bot,
	Manual
};

// Which kind of training pack the plugin is attached to
enum class PackMode
{
	None,
	Musty,    // Musty's speedflip kickoff test
	Kickoff,  // a pack with shots on the standard kickoff spawns
	Freeplay  // freeplay, with the plugin putting you on the kickoff spots
};

class SpeedFlipTrainer: public BakkesMod::Plugin::BakkesModPlugin, public BakkesMod::Plugin::PluginSettingsWindow, public BakkesMod::Plugin::PluginWindow
{
public: 

private:

	SpeedFlipTrainerMode mode = SpeedFlipTrainerMode::Manual;

	// Whether plugin is enabled
	shared_ptr<bool> enabled = make_shared<bool>(true);

	// Whether to show various meters
	shared_ptr<bool> showAngleMeter = make_shared<bool>(true);
	shared_ptr<bool> showPositionMeter = make_shared<bool>(true);
	shared_ptr<bool> showFlipMeter = make_shared<bool>(true);
	shared_ptr<bool> showJumpMeter = make_shared<bool>(true);
	shared_ptr<bool> showStickCircle = make_shared<bool>(true);

	// Stick circle placement (percent of screen) and size (percent of screen height)
	shared_ptr<float> circleX = make_shared<float>(14.0f);
	shared_ptr<float> circleY = make_shared<float>(60.0f);
	shared_ptr<float> circleSize = make_shared<float>(13.0f);

	// Most recent stick input during the current attempt (for the live dot)
	ControllerInput liveInput;

	// Whether the speed should be changed on consecutive hits and misses
	shared_ptr<bool> changeSpeed = make_shared<bool>(false);

	// What the last speed was
	shared_ptr<float> speed = make_shared<float>(1.0);

	// Whether we should remember the game speed between plugin loads
	shared_ptr<bool> rememberSpeed = make_shared<bool>(true);

	// Number of hits/misses before the game speed is changed
	shared_ptr<int> numHitsChangedSpeed = make_shared<int>(3);

	// Amount to change the game speed by
	shared_ptr<float> speedIncrement = make_shared<float>(0.1);
	
	// Optimal left angle for dodge
	shared_ptr<int> optimalLeftAngle = make_shared<int>(-30);

	// Optimal right angle for dodge
	shared_ptr<int> optimalRightAngle = make_shared<int>(30);

	// Physics ticks the flip canceled should be performed under
	shared_ptr<int> flipCancelThreshold = make_shared<int>(13);

	// Physics ticks range during which first jump should be performed
	shared_ptr<int> jumpLow = make_shared<int>(40);
	shared_ptr<int> jumpHigh = make_shared<int>(90);

	// Save attempts to file?
	shared_ptr<bool> saveToFile = make_shared<bool>(false);

	// Kickoff-spawn support
	shared_ptr<bool> kickoffEnabled = make_shared<bool>(true);
	shared_ptr<bool> kickoffAutodetect = make_shared<bool>(true);
	shared_ptr<bool> kickoffSnap = make_shared<bool>(true);
	shared_ptr<std::string> kickoffPackCode = make_shared<std::string>("");
	shared_ptr<float> kickoffSnapRadius = make_shared<float>(400.0f);

	// Time limits (seconds) to reach the ball from each spawn type
	shared_ptr<float> timeCorner = make_shared<float>(2.00f);
	shared_ptr<float> timeOffCenter = make_shared<float>(2.25f);
	shared_ptr<float> timeCenter = make_shared<float>(2.58f);

	// Musty's shot timer and car-to-ball distance, recorded when his pack runs
	shared_ptr<float> mustyTime = make_shared<float>(0.0f);
	shared_ptr<float> mustyDist = make_shared<float>(0.0f);

	PackMode packMode = PackMode::None;
	int kickoffSpawn = -1;      // index into KICKOFF_SPAWNS for this round, -1 if none
	int prepTicks = 0;          // ticks spent snapping car/ball this round
	float timeLimit = 0;        // pass/fail limit for the current attempt (seconds)
	bool speedRestored = false; // remembered game speed applied in kickoff mode

	// Freeplay kickoff practice
	shared_ptr<bool> freeplayKickoffs = make_shared<bool>(true);
	shared_ptr<int> freeplaySpot = make_shared<int>(0);          // 0 cycle, 1 random, 2.. a fixed spot
	shared_ptr<float> freeplayResetDelay = make_shared<float>(2.0f); // seconds to show the result before the next kickoff

	enum class FreeplayPhase { Waiting, Running, Done };
	FreeplayPhase freeplayPhase = FreeplayPhase::Waiting;
	int freeplayCycle = -1;          // last spot used when cycling
	int freeplayDoneFrame = 0;       // physics frame the attempt finished on
	bool freeplayNextRequested = false;
	bool freeplayHookQueued = false;

	// Whether plugin is loaded
	bool loaded = false;

	// Assists in determining when time started counting down
	float initialTime = 0;

	int startingPhysicsFrame = -1;
	int ticksBeforeTimeExpired = (int)(initialTime * 120);

	Attempt attempt;
	Attempt replayAttempt;

	BotAttempt bot;

	// Consecutive hits and misses
	int consecutiveHits = 0;
	int consecutiveMiss = 0;

	//Boilerplate
	virtual void onLoad();
	virtual void onUnload();

	// Inherited via PluginSettingsWindow
	void RenderSettings() override;
	std::string GetPluginName() override;

	// Hooks the necessary game functions to operate the plugin
	void Hook();

	// Determines whether Musty's training pack is loaded
	bool IsMustysPack(TrainingEditorWrapper tw);

	// Training pack code of the loaded pack, or "" if none
	std::string GetPackCode(TrainingEditorWrapper tw);

	// Kickoff mode: find the spawn this round uses and snap car/ball onto it
	void PrepareKickoffRound(CarWrapper car);

	// Kickoff mode: keep the shot clock at the limit and end the shot when it runs out
	void EnforceKickoffLimit(int currentFrame, float timeLeft);

	// Time limit for a spawn (seconds)
	float LimitForSpawn(const KickoffSpawn& s);

	// Whether the last attempt counts as a hit for the speed-change streaks
	bool AttemptSucceeded();

	// Whether there is anything to measure/draw this round
	bool RoundActive();

	// Kickoff pack or freeplay: measured from a kickoff spawn with a time limit
	bool KickoffLike() { return packMode == PackMode::Kickoff || packMode == PackMode::Freeplay; }

	// Whether the game is in the mode the plugin is currently attached to
	bool InActiveMode();

	// Streaks, game-speed changes and saving once an attempt is over
	void FinishAttempt();

	// Freeplay: choose the next kickoff spot and wait for the player to go
	void StartFreeplayKickoff();

	// Freeplay: per-tick handling. Returns false if the tick shouldn't be measured.
	bool FreeplayTick(CarWrapper car, ControllerInput* input, int currentFrame);

	// Freeplay: big good/too slow banner
	void RenderFreeplayResult(CanvasWrapper canvas, float screenWidth, float screenHeight);

	// Measures the speedflip being performed
	void Measure(CarWrapper car, shared_ptr<GameWrapper> gameWrapper);

	// Programmed bot to perform the speed flip
	void PlayBot(shared_ptr<GameWrapper> gameWrapper, ControllerInput* ci);

	// Replays saved speedflip attempt
	void PlayAttempt(Attempt* a, shared_ptr<GameWrapper> gameWrapper, ControllerInput* ci);

	// Render functions to render various meters and measured values on screen
	void RenderMeters(CanvasWrapper canvas);
	void RenderAngleMeter(CanvasWrapper canvas, float screenWidth, float screenHeight);
	void RenderFlipCancelMeter(CanvasWrapper canvas, float screenWidth, float screenHeight);
	void RenderFirstJumpMeter(CanvasWrapper canvas, float screenWidth, float screenHeight);
	void RenderPositionMeter(CanvasWrapper canvas, float screenWidth, float screenHeight);
	void RenderStickCircle(CanvasWrapper canvas, float screenWidth, float screenHeight);

	// Inherited via PluginWindow
	bool isWindowOpen_ = false;
	bool isMinimized_ = false;
	std::string menuTitle_ = "Speedflip Trainer";

	virtual void Render() override;
	virtual std::string GetMenuName() override;
	virtual std::string GetMenuTitle() override;
	virtual void SetImGuiContext(uintptr_t ctx) override;
	virtual bool ShouldBlockInput() override;
	virtual bool IsActiveOverlay() override;
	virtual void OnOpen() override;
	virtual void OnClose() override;

	filesystem::path dataDir;
	ImGui::FileDialog attemptFileDialog;
	ImGui::FileDialog botFileDialog;
};

