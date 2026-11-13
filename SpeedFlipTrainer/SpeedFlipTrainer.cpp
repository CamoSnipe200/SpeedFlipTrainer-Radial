#include "pch.h"	
#include "SpeedFlipTrainer.h"
#include "RenderMeter.h"
#include <array>
#include <cctype>
#include "BotAttempt.h"

#define _USE_MATH_DEFINES
#include <math.h>

BAKKESMOD_PLUGIN(SpeedFlipTrainer, "Speedflip trainer", plugin_version, PLUGINTYPE_CUSTOM_TRAINING)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

struct clock_time {
	int hour_hand;
	int min_hand;
};

int ComputeDodgeAngle(DodgeComponentWrapper dodge)
{
	if (dodge.IsNull())
		return 0;

	Vector dd = dodge.GetDodgeDirection();
	if (dd.X == 0 && dd.Y == 0)
		return 0;

	//LOG("X: {0} Y: {1}", dd.X, dd.Y);
	return (int)(atan2f(dd.Y, dd.X) * (180 / M_PI));
}

clock_time ComputeClockTime(int angle)
{
	if (angle < 0)
	{
		angle = 360 + angle;
	}
	clock_time time;

	time.hour_hand = (int)(angle * (12.0 / 360.0));
	time.min_hand = (angle % (360 / 12)) * (60 / 30);

	return time;
}

float distance(Vector a, Vector b)
{
	// Calculating distance
	return sqrt(pow(a.X - b.X, 2) +
		pow(a.Y - b.Y, 2));
}

void SpeedFlipTrainer::Measure(CarWrapper car, std::shared_ptr<GameWrapper> gameWrapper)
{
	int currentPhysicsFrame = gameWrapper->GetEngine().GetPhysicsFrame();
	int currentTick = currentPhysicsFrame - startingPhysicsFrame;

	ControllerInput input = car.GetInput();
	attempt.Record(currentTick, input);

	// Sideways drift. Musty's shot drives along X, so that's just Y.
	// For kickoff spawns it's the offset from the spawn -> ball line.
	auto loc = car.GetLocation();
	float lateral = loc.Y;
	if (KickoffLike() && kickoffSpawn >= 0)
	{
		const KickoffSpawn& s = KICKOFF_SPAWNS[kickoffSpawn];
		float len = sqrtf(s.x * s.x + s.y * s.y);
		float dx = -s.x / len, dy = -s.y / len;              // unit vector spawn -> ball
		float right = (loc.X - s.x) * (-dy) + (loc.Y - s.y) * dx; // offset to the car's right
		lateral = -right;                                     // position meter draws -positionY
	}
	attempt.traveledY += abs(lateral - attempt.positionY);
	attempt.positionY = lateral;

	if (!attempt.jumped && car.GetbJumped())
	{
		attempt.jumped = true;
		attempt.jumpTick = currentTick;
		LOG("First jump: " + to_string(currentTick) + " ticks");
	}

	// Count jump-button presses; on the second one, freeze where the stick is
	bool jumpHeld = input.Jump != 0;
	if (jumpHeld && !attempt.prevJumpHeld)
	{
		attempt.jumpPresses++;
		if (attempt.jumpPresses == 2 && !attempt.stickRecorded)
		{
			attempt.stickRecorded = true;
			attempt.stickX = input.Steer;
			attempt.stickY = -input.Pitch;
			attempt.stickAngle = (int)lroundf(atan2f(attempt.stickX, attempt.stickY) * (180.0f / (float)M_PI));
			LOG("Stick at second jump: {0} deg (steer {1:.2f}, pitch {2:.2f}, yaw {3:.2f}, roll {4:.2f})",
				attempt.stickAngle, input.Steer, input.Pitch, input.Yaw, input.Roll);
		}
	}
	attempt.prevJumpHeld = jumpHeld;

	if (!attempt.dodged && car.IsDodging())
	{
		attempt.dodged = true;
		attempt.dodgedTick = currentTick;
		auto dodge = car.GetDodgeComponent();
		if (!dodge.IsNull())
		{
			attempt.dodgeAngle = ComputeDodgeAngle(dodge);
			clock_time time = ComputeClockTime(attempt.dodgeAngle);
			string msg = fmt::format("Dodge Angle: {0:#03d} deg or {1:#02d}:{2:#02d} PM", attempt.dodgeAngle, time.hour_hand, time.min_hand);
			LOG(msg);
		}
	}

	if (input.Throttle != 1)
		attempt.ticksNotPressingThrottle++;
	if (input.ActivateBoost != 1)
		attempt.ticksNotPressingBoost++;

	if (!attempt.flipCanceled  && attempt.dodged && input.Pitch > 0.8)
	{
		attempt.flipCanceled = true;
		attempt.flipCancelTick = currentTick;
		LOG("Flip Cancel: " + to_string(attempt.flipCancelTick - attempt.dodgedTick) + " ticks");
	}
}

void SpeedFlipTrainer::Hook()
{
	if (loaded)
		return;
	loaded = true;

	LOG("Hooking");
	gameWrapper->RegisterDrawable(bind(&SpeedFlipTrainer::RenderMeters, this, std::placeholders::_1));

	// In kickoff mode this waits until a round is actually on a kickoff spawn,
	// so auto-detect doesn't slow down unrelated packs
	if (*rememberSpeed && (packMode == PackMode::Musty || packMode == PackMode::Freeplay))
		_globalCvarManager->getCvar("sv_soccar_gamespeed").setValue(*speed);

	gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper car, void* params, std::string eventname) {
		if (!*enabled || !loaded || !InActiveMode())
			return;

		if (car.IsNull())
			return;

		// Kickoff mode: snap car/ball for the first few ticks of the round
		if (packMode == PackMode::Kickoff && startingPhysicsFrame < 0 && prepTicks < KICKOFF_PREP_TICKS)
		{
			PrepareKickoffRound(car);
			prepTicks++;
		}
		if (packMode == PackMode::Kickoff && kickoffSpawn < 0)
			return; // not a kickoff shot

		if (*rememberSpeed && (packMode == PackMode::Musty || packMode == PackMode::Freeplay || speedRestored))
			*(this->speed) = _globalCvarManager->getCvar("sv_soccar_gamespeed").getFloatValue();

		auto input = (ControllerInput*)params;

		if(mode == SpeedFlipTrainerMode::Bot)
			PlayBot(gameWrapper, input);
		else if (mode == SpeedFlipTrainerMode::Replay)
			PlayAttempt(&replayAttempt, gameWrapper, input);

		// Remember the latest stick position for the stick circle's live dot
		liveInput = *input;

		int currentFrame = gameWrapper->GetEngine().GetPhysicsFrame();

		// Freeplay has no shot clock: the plugin places you and times from your first input
		if (packMode == PackMode::Freeplay)
		{
			if (FreeplayTick(car, input, currentFrame) && !attempt.hit)
				Measure(car, gameWrapper);
			return;
		}

		// Has time started counting down?
		float timeLeft = gameWrapper->GetCurrentGameState().GetGameTimeRemaining();
		if (startingPhysicsFrame < 0)
		{
			if (initialTime <= 0) // no time on the clock
				return;
			if (timeLeft > initialTime) // game reset the clock after Restart
			{
				initialTime = timeLeft;
				return;
			}
			if (timeLeft == initialTime) // countdown hasn't started
				return;

			// first frame the countdown has begun
			startingPhysicsFrame = currentFrame;

			attempt = Attempt();

			if (!car.IsOnGround())
				attempt.startedInAir = true;

			if (!input->ActivateBoost)
				attempt.startedNoBoost = true;

			if (packMode == PackMode::Kickoff)
			{
				attempt.positionY = 0;
				timeLimit = LimitForSpawn(KICKOFF_SPAWNS[kickoffSpawn]);
				LOG("Kickoff attempt from {0}, limit {1:.2f}s", KICKOFF_SPAWNS[kickoffSpawn].name, timeLimit);
			}
			else
			{
				timeLimit = initialTime;

				// Remember Musty's timer and distance so kickoff limits can be calibrated to it
				ServerWrapper server = gameWrapper->GetGameEventAsServer();
				if (!server.IsNull())
				{
					BallWrapper ball = server.GetBall();
					if (!ball.IsNull())
					{
						float d = distance(car.GetLocation(), ball.GetLocation());
						if (d > 2500)
						{
							_globalCvarManager->getCvar("sf_musty_time").setValue(initialTime);
							_globalCvarManager->getCvar("sf_musty_dist").setValue(d);
							LOG("Musty's shot: {0:.3f}s over {1:.0f}uu -> corner {2:.2f}s, off-center {3:.2f}s, back center {4:.2f}s",
								initialTime, d,
								initialTime + (KICKOFF_DIST_CORNER - d) / KICKOFF_MAX_SPEED,
								initialTime + (KICKOFF_DIST_OFFCENTER - d) / KICKOFF_MAX_SPEED,
								initialTime + (KICKOFF_DIST_CENTER - d) / KICKOFF_MAX_SPEED);
						}
					}
				}
			}
		}

		if (packMode == PackMode::Kickoff && !attempt.hit && !attempt.exploded)
			EnforceKickoffLimit(currentFrame, timeLeft);

		// ball hasn't exploded or been hit yet
		if (!attempt.exploded && !attempt.hit)
		{
			Measure(car, gameWrapper);
		}
	});

	gameWrapper->HookEvent("Function TAGame.Ball_TA.RecordCarHit",
		[this](std::string eventname) {
		if (!*enabled || !loaded || !InActiveMode() || attempt.hit || attempt.exploded)
			return;
		if (!RoundActive() || startingPhysicsFrame < 0)
			return;
		if (packMode == PackMode::Freeplay && freeplayPhase != FreeplayPhase::Running)
			return;

		attempt.ticksToBall = gameWrapper->GetLocalCar().GetLastBallImpactFrame() - startingPhysicsFrame;
		attempt.hit = true;
		attempt.reached = true;
		if (KickoffLike())
		{
			// The clock is being clamped, so time it from physics ticks
			attempt.timeToBall = attempt.ticksToBall / 120.0f;
			attempt.passed = !attempt.timedOut && attempt.ticksToBall <= (int)lroundf(timeLimit * 120.0f);
		}
		else
		{
			attempt.timeToBall = initialTime - gameWrapper->GetCurrentGameState().GetGameTimeRemaining();
			attempt.passed = true; // the pack's own timer is the limit: a touch means you made it
		}
		LOG("Time to ball: {0:.3f}s after {1} tick", attempt.timeToBall, attempt.ticksToBall);

		if (packMode == PackMode::Freeplay && freeplayPhase == FreeplayPhase::Running)
		{
			freeplayPhase = FreeplayPhase::Done;
			freeplayDoneFrame = gameWrapper->GetEngine().GetPhysicsFrame();
			FinishAttempt();
		}
	});

	gameWrapper->HookEvent("Function TAGame.Ball_TA.Explode", 
		[this](std::string eventName) {
		if (!*enabled || !loaded || !gameWrapper->IsInCustomTraining() || !RoundActive())
			return;

		auto ball = gameWrapper->GetGameEventAsServer().GetBall();
		auto car = gameWrapper->GetGameEventAsServer().GetLocalPrimaryPlayer();
		if (ball.IsNull() || car.IsNull())
		{
			attempt.exploded = true;
			attempt.hit = false;
			return;
		}

		auto distanceToBall = distance(ball.GetLocation(), car.GetLocation());
		auto meters = (distanceToBall / 100.0) - 4.8; // account for car distance
		LOG("Distance to ball = {0:.1f}m", meters);

		attempt.exploded = true;
		attempt.hit = false;
	});

	gameWrapper->HookEventPost("Function Engine.Controller.Restart", 
		[this](std::string eventName) {
		if (!*enabled || !loaded || !gameWrapper->IsInCustomTraining())
			return;

		initialTime = gameWrapper->GetCurrentGameState().GetGameTimeRemaining();
		ticksBeforeTimeExpired = initialTime * 120;

		// A reset in kickoff mode on a non-kickoff shot (or before the car moved) isn't an attempt
		bool countIt = RoundActive() && startingPhysicsFrame >= 0;
		startingPhysicsFrame = -1;
		if (packMode == PackMode::Kickoff)
		{
			prepTicks = 0;
			kickoffSpawn = -1;
		}
		if (!countIt && packMode == PackMode::Kickoff)
			return;

		FinishAttempt();
	});
}

void SpeedFlipTrainer::FinishAttempt()
{
	if (AttemptSucceeded())
	{
		consecutiveHits++;
		consecutiveMiss = 0;
	}
	else
	{
		consecutiveHits = 0;
		consecutiveMiss++;
	}

	if (*saveToFile && attempt.inputs.size() > 0)
	{
		auto path = attempt.GetFilename(dataDir);
		attempt.WriteInputsToFile(path);
		LOG("Saving attempt to: {0}", path.string());
	}

	auto speedCvar = _globalCvarManager->getCvar("sv_soccar_gamespeed");
	float speed = speedCvar.getFloatValue();

	if (*changeSpeed)
	{
		if (consecutiveHits != 0 && consecutiveHits % (*numHitsChangedSpeed) == 0)
		{
			gameWrapper->LogToChatbox(to_string(consecutiveHits) + " " + (consecutiveHits > 1 ? "hits" : "hit") + " in a row");
			speed += *speedIncrement;
			speedCvar.setValue(speed);
			string msg = fmt::format("Game speed + {0:.3f} = {1:.3f}", *speedIncrement, speed);
			gameWrapper->LogToChatbox(msg);
		}
		else if (consecutiveMiss != 0 && consecutiveMiss % (*numHitsChangedSpeed) == 0)
		{
			gameWrapper->LogToChatbox(to_string(consecutiveMiss) + " " + (consecutiveMiss > 1 ? "misses" : "miss") + " in a row");
			speed -= *speedIncrement;
			if (speed <= 0)
				speed = 0;
			speedCvar.setValue(speed);
			string msg = fmt::format("Game speed - {0:.3f} = {1:.3f}", *speedIncrement, speed);
			gameWrapper->LogToChatbox(msg);
		}
	}
}

void SpeedFlipTrainer::onLoad()
{
	_globalCvarManager = cvarManager;

	cvarManager->registerCvar("sf_enabled", "1", "Enabled speedflip training.", true, false, 0, false, 0, true).bindTo(enabled);
	cvarManager->getCvar("sf_enabled").addOnValueChanged([this](string oldVal, CVarWrapper cvar)
	{
		LOG("sf_enabled = {0}", *enabled);
		if (!*enabled)
		{
			onUnload();
		}
	});

	cvarManager->registerCvar("sf_save_attempts", "0", "Save attmempts to a file.", true, false, 0, false, 0, true).bindTo(saveToFile);

	cvarManager->registerCvar("sf_change_speed", "0", "Change game speed on consecutive hits and misses.", true, false, 0, false, 0, true).bindTo(changeSpeed);
	cvarManager->registerCvar("sf_speed", "1.0", "Change game speed on consecutive hits and misses.", true, false, 0.0, false, 1.0, true).bindTo(speed);
	cvarManager->registerCvar("sf_remember_speed", "1", "Remember last set speed.", true, true, 1, true, 1, true).bindTo(rememberSpeed);
	cvarManager->registerCvar("sf_num_hits", "3", "Number of hits/misses before the speed increases/decreases.", true, true, 0, true, 50, true).bindTo(numHitsChangedSpeed);
	cvarManager->registerCvar("sf_speed_increment", "0.05", "Speed increase/decrease increment.", true, true, 0.001, true, 0.999, true).bindTo(speedIncrement);

	cvarManager->registerCvar("sf_left_angle", "-30", "Optimal left angle", true, true, -78, true, -12, true).bindTo(optimalLeftAngle);
	cvarManager->registerCvar("sf_right_angle", "30", "Optimal right angle", true, true, 12, true, 78, true).bindTo(optimalRightAngle);
	cvarManager->registerCvar("sf_cancel_threshold", "10", "Optimal flip cancel threshold.", true, true, 1, true, 15, true).bindTo(flipCancelThreshold);

	cvarManager->registerCvar("sf_show_angle", "1", "Show angle meter.", true, false, 0, false, 0, true).bindTo(showAngleMeter);
	cvarManager->registerCvar("sf_show_position", "1", "Show horizontal position meter.", true, false, 0, false, 0, true).bindTo(showPositionMeter);
	cvarManager->registerCvar("sf_show_jump", "1", "Show jump meter.", true, false, 0, false, 0, true).bindTo(showJumpMeter);
	cvarManager->registerCvar("sf_show_flip", "1", "Show flip cancel meter.", true, false, 0, false, 0, true).bindTo(showFlipMeter);
	cvarManager->registerCvar("sf_show_circle", "1", "Show stick angle circle.", true, false, 0, false, 0, true).bindTo(showStickCircle);
	cvarManager->registerCvar("sf_circle_x", "14", "Stick circle horizontal position (% of screen width).", true, true, 0, true, 100, true).bindTo(circleX);
	cvarManager->registerCvar("sf_circle_y", "60", "Stick circle vertical position (% of screen height).", true, true, 0, true, 100, true).bindTo(circleY);
	cvarManager->registerCvar("sf_circle_size", "13", "Stick circle radius (% of screen height).", true, true, 5, true, 30, true).bindTo(circleSize);

	cvarManager->registerCvar("sf_kickoff_enabled", "1", "Support training packs with shots on the standard kickoff spawns.", true, false, 0, false, 0, true).bindTo(kickoffEnabled);
	cvarManager->registerCvar("sf_kickoff_autodetect", "1", "Treat any training shot that starts on a kickoff spawn (ball at center) as a kickoff shot.", true, false, 0, false, 0, true).bindTo(kickoffAutodetect);
	cvarManager->registerCvar("sf_kickoff_pack_code", "", "Code of your kickoff-spawns training pack.", true, false, 0, false, 0, true).bindTo(kickoffPackCode);
	cvarManager->registerCvar("sf_kickoff_snap", "1", "Snap the car and ball to the exact kickoff spot.", true, false, 0, false, 0, true).bindTo(kickoffSnap);
	cvarManager->registerCvar("sf_kickoff_snap_radius", "400", "How close (uu) the car must start to a kickoff spawn.", true, true, 50, true, 1000, true).bindTo(kickoffSnapRadius);
	cvarManager->registerCvar("sf_kickoff_time_corner", "2.00", "Time limit (s) to reach the ball from a corner kickoff.", true, true, 1.0, true, 5.0, true).bindTo(timeCorner);
	cvarManager->registerCvar("sf_kickoff_time_offcenter", "2.25", "Time limit (s) to reach the ball from an off-center kickoff.", true, true, 1.0, true, 5.0, true).bindTo(timeOffCenter);
	cvarManager->registerCvar("sf_kickoff_time_center", "2.58", "Time limit (s) to reach the ball from the back-center kickoff.", true, true, 1.0, true, 5.0, true).bindTo(timeCenter);
	cvarManager->registerCvar("sf_musty_time", "0", "Shot timer of Musty's pack (recorded automatically).", true, false, 0, false, 0, true).bindTo(mustyTime);
	cvarManager->registerCvar("sf_musty_dist", "0", "Car-to-ball distance in Musty's pack (recorded automatically).", true, false, 0, false, 0, true).bindTo(mustyDist);

	cvarManager->registerCvar("sf_freeplay_kickoffs", "1", "In freeplay, put the car on the kickoff spots and time speedflips to the ball.", true, false, 0, false, 0, true).bindTo(freeplayKickoffs);
	cvarManager->getCvar("sf_freeplay_kickoffs").addOnValueChanged([this](string oldVal, CVarWrapper cvar)
	{
		if (!*freeplayKickoffs && packMode == PackMode::Freeplay)
			onUnload();
	});
	cvarManager->registerCvar("sf_freeplay_spot", "0", "Freeplay kickoff spot: 0 cycle, 1 random, 2 right corner, 3 left corner, 4 back right, 5 back left, 6 back center.", true, true, 0, true, 6, true).bindTo(freeplaySpot);
	cvarManager->registerCvar("sf_freeplay_reset_delay", "2.0", "Seconds to show the result before the next freeplay kickoff.", true, true, 0.5, true, 10.0, true).bindTo(freeplayResetDelay);
	cvarManager->registerNotifier("sf_freeplay_next", [this](std::vector<std::string> args) {
		freeplayNextRequested = true;
	}, "Go to the next freeplay kickoff now.", PERMISSION_ALL);

	//cvarManager->registerCvar("sf_jump_low", "40", "Low threshold for first jump of speedflip.", true, true, 10, true, 110, false).bindTo(jumpLow);
	//cvarManager->registerCvar("sf_jump_high", "90", "High threshold for first jump of speedflip.", true, true, 20, true, 120, false).bindTo(jumpHigh);

	if (*enabled)
	{
		gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.LoadRound", [this](ActorWrapper cw, void* params, std::string eventName) {
			if (!*enabled)
				return;

			std::string code = GetPackCode((TrainingEditorWrapper)cw.memory_address);
			if (code == "A503-264C-A7EB-D282")
			{
				packMode = PackMode::Musty;
			}
			else if (*kickoffEnabled)
			{
				auto normalize = [](std::string s) {
					std::string out;
					for (char c : s)
						if (!isspace((unsigned char)c))
							out += (char)toupper((unsigned char)c);
					return out;
				};
				std::string mine = normalize(*kickoffPackCode);
				bool isMine = !mine.empty() && normalize(code) == mine;
				if (!isMine && !*kickoffAutodetect)
					return;
				packMode = PackMode::Kickoff;
			}
			else
			{
				return;
			}
			Hook();
		});

		gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.Destroyed", [this](ActorWrapper cw, void* params, std::string eventName) {
			if(loaded)
				onUnload();
		});

		// Freeplay has no "round loaded" event we can rely on, so watch car input:
		// attach when freeplay starts, detach when we're somewhere else
		gameWrapper->HookEventWithCallerPost<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput", [this](CarWrapper car, void* params, std::string eventName) {
			if (freeplayHookQueued)
				return;
			bool inFreeplay = gameWrapper->IsInFreeplay();
			bool wantFreeplay = *enabled && *freeplayKickoffs && inFreeplay;
			if (!loaded && wantFreeplay)
			{
				// (un)hooking from inside a hook isn't safe, so do it right after
				freeplayHookQueued = true;
				gameWrapper->SetTimeout([this](GameWrapper* gw) {
					freeplayHookQueued = false;
					if (loaded || !*enabled || !*freeplayKickoffs || !gameWrapper->IsInFreeplay())
						return;
					packMode = PackMode::Freeplay;
					Hook();
					StartFreeplayKickoff();
				}, 0.01f);
			}
			else if (loaded && packMode == PackMode::Freeplay && !inFreeplay)
			{
				freeplayHookQueued = true;
				gameWrapper->SetTimeout([this](GameWrapper* gw) {
					freeplayHookQueued = false;
					if (loaded && packMode == PackMode::Freeplay)
						onUnload();
				}, 0.01f);
			}
		});

		dataDir = gameWrapper->GetDataFolder().append("speedflip");
		if (!std::filesystem::exists(dataDir))
			std::filesystem::create_directories(dataDir);

		attemptFileDialog.workingDirectory = dataDir / "attempts";
		if (!std::filesystem::exists(attemptFileDialog.workingDirectory))
			std::filesystem::create_directories(attemptFileDialog.workingDirectory);
		attemptFileDialog.name = "Select replay attempt";

		botFileDialog.workingDirectory = dataDir / "bots";
		if (!std::filesystem::exists(botFileDialog.workingDirectory))
			std::filesystem::create_directories(botFileDialog.workingDirectory);
		botFileDialog.name = "Select bot file";
	}

}

void SpeedFlipTrainer::onUnload()
{
	if (!loaded)
		return;
	loaded = false;

	LOG("Unhooking");
	gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
	gameWrapper->UnhookEvent("Function TAGame.Ball_TA.RecordCarHit");
	gameWrapper->UnhookEvent("Function TAGame.Ball_TA.Explode");
	gameWrapper->UnhookEventPost("Function Engine.Controller.Restart"); 
	gameWrapper->UnregisterDrawables();

	packMode = PackMode::None;
	kickoffSpawn = -1;
	prepTicks = 0;
	speedRestored = false;
	freeplayPhase = FreeplayPhase::Waiting;
	freeplayNextRequested = false;
}

std::string SpeedFlipTrainer::GetPackCode(TrainingEditorWrapper tw)
{
	if (!tw.IsNull())
	{
		GameEditorSaveDataWrapper data = tw.GetTrainingData();
		if (!data.IsNull())
		{
			TrainingEditorSaveDataWrapper td = data.GetTrainingData();
			if (!td.IsNull() && !td.GetCode().IsNull())
				return td.GetCode().ToString();
		}
	}
	return "";
}

bool SpeedFlipTrainer::IsMustysPack(TrainingEditorWrapper tw)
{
	return GetPackCode(tw) == "A503-264C-A7EB-D282";
}

bool SpeedFlipTrainer::RoundActive()
{
	if (KickoffLike())
		return kickoffSpawn >= 0;
	return packMode == PackMode::Musty;
}

bool SpeedFlipTrainer::InActiveMode()
{
	if (packMode == PackMode::Freeplay)
		return gameWrapper->IsInFreeplay();
	return gameWrapper->IsInCustomTraining();
}

void SpeedFlipTrainer::StartFreeplayKickoff()
{
	// Blue-side spawns are the first five entries
	const int blueSpots = 5;
	int spot = *freeplaySpot;
	int idx;
	if (spot >= 2 && spot - 2 < blueSpots)
		idx = spot - 2;
	else if (spot == 1)
		idx = rand() % blueSpots;
	else
		idx = freeplayCycle = (freeplayCycle + 1) % blueSpots;

	kickoffSpawn = idx;
	freeplayPhase = FreeplayPhase::Waiting;
	freeplayNextRequested = false;
	startingPhysicsFrame = -1;
	LOG("Freeplay kickoff: {0}", KICKOFF_SPAWNS[idx].name);
}

bool SpeedFlipTrainer::FreeplayTick(CarWrapper car, ControllerInput* input, int currentFrame)
{
	if (freeplayNextRequested || kickoffSpawn < 0)
		StartFreeplayKickoff();

	const KickoffSpawn& s = KICKOFF_SPAWNS[kickoffSpawn];

	if (freeplayPhase == FreeplayPhase::Waiting)
	{
		bool go = input->Throttle > 0.01f || input->ActivateBoost;
		if (!go)
		{
			// Hold the car on the spot and the ball on center until the player goes
			ServerWrapper server = gameWrapper->GetGameEventAsServer();
			if (server.IsNull())
				return false;
			BallWrapper ball = server.GetBall();
			if (!ball.IsNull())
			{
				ball.SetLocation(Vector(0, 0, 92.75f));
				ball.SetVelocity(Vector(0, 0, 0));
				ball.SetAngularVelocity(Vector(0, 0, 0), false);
			}
			car.SetLocation(Vector(s.x, s.y, 17.01f));
			car.SetCarRotation(Rotator(0, s.yaw, 0));
			car.SetVelocity(Vector(0, 0, 0));
			car.SetAngularVelocity(Vector(0, 0, 0), false);
			return false;
		}

		// First throttle/boost input starts the clock, like a real kickoff
		freeplayPhase = FreeplayPhase::Running;
		startingPhysicsFrame = currentFrame;
		attempt = Attempt();
		attempt.positionY = 0;
		if (!input->ActivateBoost)
			attempt.startedNoBoost = true;
		timeLimit = LimitForSpawn(s);
		return true;
	}

	if (freeplayPhase == FreeplayPhase::Running)
	{
		int elapsed = currentFrame - startingPhysicsFrame;
		int limitTicks = (int)lroundf(timeLimit * 120.0f);
		if (elapsed > limitTicks && !attempt.timedOut)
		{
			// Too slow: say so now, but let them keep going to see their time
			attempt.timedOut = true;
			LOG("Too slow: no touch within {0:.2f}s", timeLimit);
		}
		if (elapsed > limitTicks + 2 * 120)
		{
			// Never got there; move on
			freeplayPhase = FreeplayPhase::Done;
			freeplayDoneFrame = currentFrame;
			FinishAttempt();
			return false;
		}
		return true;
	}

	// Done: show the result for a bit, then the next kickoff
	if (currentFrame - freeplayDoneFrame > (int)(*freeplayResetDelay * 120.0f))
		StartFreeplayKickoff();
	return false;
}

void SpeedFlipTrainer::RenderFreeplayResult(CanvasWrapper canvas, float screenWidth, float screenHeight)
{
	if (kickoffSpawn < 0)
		return;

	string msg;
	bool good = false;
	if (freeplayPhase == FreeplayPhase::Waiting)
	{
		const KickoffSpawn& s = KICKOFF_SPAWNS[kickoffSpawn];
		msg = fmt::format("{0} - go when ready (limit {1:.2f}s)", s.name, LimitForSpawn(s));
	}
	else if (attempt.reached && attempt.ticksToBall > 0)
	{
		good = attempt.passed;
		msg = fmt::format("{0} - {1:.3f}s", good ? "GOOD SPEEDFLIP" : "TOO SLOW", attempt.timeToBall);
	}
	else if (attempt.timedOut)
	{
		msg = fmt::format("TOO SLOW - limit {0:.2f}s", timeLimit);
	}
	if (msg.empty())
		return;

	float scale = freeplayPhase == FreeplayPhase::Waiting ? 2.0f : 3.0f;
	Vector2F size = canvas.GetStringSize(msg, scale, scale);
	if (freeplayPhase == FreeplayPhase::Waiting)
		canvas.SetColor(255, 255, 255, 255);
	else if (good)
		canvas.SetColor(50, 255, 50, 255);
	else
		canvas.SetColor(255, 10, 10, 255);
	canvas.SetPosition(Vector2F{ (screenWidth - size.X) / 2.0f, screenHeight * 0.22f });
	canvas.DrawString(msg, scale, scale, true, false);
}

bool SpeedFlipTrainer::AttemptSucceeded()
{
	if (KickoffLike())
		return attempt.passed;
	return attempt.hit && !attempt.exploded;
}

float SpeedFlipTrainer::LimitForSpawn(const KickoffSpawn& s)
{
	switch (s.type)
	{
	case KickoffType::Corner: return *timeCorner;
	case KickoffType::OffCenter: return *timeOffCenter;
	default: return *timeCenter;
	}
}

void SpeedFlipTrainer::PrepareKickoffRound(CarWrapper car)
{
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull())
		return;
	BallWrapper ball = server.GetBall();
	if (ball.IsNull())
		return;

	// Nearest kickoff spawn to where the shot put the car
	Vector loc = car.GetLocation();
	int best = -1;
	float bestDist = 1e9f;
	for (int i = 0; i < (int)KICKOFF_SPAWNS.size(); i++)
	{
		float d = sqrtf(powf(loc.X - KICKOFF_SPAWNS[i].x, 2) + powf(loc.Y - KICKOFF_SPAWNS[i].y, 2));
		if (d < bestDist)
		{
			bestDist = d;
			best = i;
		}
	}

	Vector b = ball.GetLocation();
	float ballOff = sqrtf(b.X * b.X + b.Y * b.Y);
	if (best < 0 || bestDist > *kickoffSnapRadius || ballOff > 500.0f)
	{
		kickoffSpawn = -1;
		if (prepTicks == 0 && best >= 0)
			LOG("Not a kickoff shot: nearest spawn {0} is {1:.0f}uu away, ball {2:.0f}uu from center", KICKOFF_SPAWNS[best].name, bestDist, ballOff);
		return;
	}

	const KickoffSpawn& s = KICKOFF_SPAWNS[best];
	kickoffSpawn = best;

	if (prepTicks == 0)
		LOG("Kickoff spawn: {0}, was {1:.0f}uu off", s.name, bestDist);

	if (!speedRestored)
	{
		speedRestored = true;
		if (*rememberSpeed)
			_globalCvarManager->getCvar("sv_soccar_gamespeed").setValue(*speed);
	}

	if (*kickoffSnap)
	{
		// Keep Z so the car still settles onto the ground normally
		car.SetLocation(Vector(s.x, s.y, loc.Z));
		car.SetCarRotation(Rotator(0, s.yaw, 0));
		Vector v = car.GetVelocity();
		car.SetVelocity(Vector(0, 0, v.Z));
		car.SetAngularVelocity(Vector(0, 0, 0), false);

		ball.SetLocation(Vector(0, 0, b.Z));
		ball.SetVelocity(Vector(0, 0, 0));
		ball.SetAngularVelocity(Vector(0, 0, 0), false);
	}
}

void SpeedFlipTrainer::EnforceKickoffLimit(int currentFrame, float timeLeft)
{
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (server.IsNull())
		return;

	int elapsed = currentFrame - startingPhysicsFrame;
	int limitTicks = (int)lroundf(timeLimit * 120.0f);

	if (elapsed >= limitTicks)
	{
		if (!attempt.timedOut)
		{
			// Let the game end the shot its own way (ball explodes)
			attempt.timedOut = true;
			server.SetGameTimeRemaining(0.0f);
			LOG("Too slow: no touch within {0:.2f}s", timeLimit);
		}
		else if (elapsed >= limitTicks + KICKOFF_EXPLODE_GRACE_TICKS && !attempt.exploded)
		{
			// The game didn't end it, so make the ball disappear ourselves
			BallWrapper ball = server.GetBall();
			if (!ball.IsNull())
				ball.DoExplode();
			attempt.exploded = true;
		}
	}
	else
	{
		// Keep the shot clock showing the time left to make it
		float want = (limitTicks - elapsed) / 120.0f;
		if (timeLeft > want + 0.02f)
			server.SetGameTimeRemaining(want);
	}
}

void SpeedFlipTrainer::RenderMeters(CanvasWrapper canvas)
{
	if (!*enabled || !loaded || !InActiveMode() || !RoundActive()) return;

	float SCREENWIDTH = canvas.GetSize().X;
	float SCREENHEIGHT = canvas.GetSize().Y;

	if (packMode == PackMode::Freeplay)
		RenderFreeplayResult(canvas, SCREENWIDTH, SCREENHEIGHT);

	if(*showAngleMeter)
		RenderAngleMeter(canvas, SCREENWIDTH, SCREENHEIGHT);

	if(*showPositionMeter)
		RenderPositionMeter(canvas, SCREENWIDTH, SCREENHEIGHT);

	if(*showFlipMeter)
		RenderFlipCancelMeter(canvas, SCREENWIDTH, SCREENHEIGHT);

	if(*showJumpMeter)
		RenderFirstJumpMeter(canvas, SCREENWIDTH, SCREENHEIGHT);

	if(*showStickCircle)
		RenderStickCircle(canvas, SCREENWIDTH, SCREENHEIGHT);
}

void SpeedFlipTrainer::RenderPositionMeter(CanvasWrapper canvas, float screenWidth, float screenHeight)
{
	float mid = -1.1;
	int range = 200;
	int relLocation = (-1 * attempt.positionY) + range;
	int totalUnits = range * 2;

	float opacity = 1.0;
	Vector2 reqSize = Vector2{ (int)(screenWidth * 70 / 100.f), (int)(screenHeight * 4 / 100.f) };
	int unitWidth = reqSize.X / totalUnits;

	Vector2 boxSize = Vector2{ unitWidth * totalUnits, reqSize.Y };
	Vector2 startPos = Vector2{ (int)((screenWidth/2) - (boxSize.X/2)), (int)(screenHeight * 10 / 100.f) };

	struct Color baseColor = { (char)255, (char)255, (char)255, opacity };
	struct Line border = { (char)255, (char)255, (char)255, opacity, 2 };

	std::list<MeterRange> ranges;
	if (startingPhysicsFrame > 0)
	{
		float go = 1, ro = 1, yo = 1;
		if (relLocation >= range - 80 && relLocation <= range + 80)
		{
			ranges.push_back({ (char)50, (char)255, (char)50, go, range - 80, range + 80 });
		}
		else if (relLocation >= range - 160 && relLocation <= range + 160)
		{
			ranges.push_back({ (char)255, (char)255, (char)50, yo,  range - 160, range + 160 });
			ranges.push_back({ (char)255, (char)255, (char)50, yo,  range - 160, range + 160 });
		}
		else
		{
			ranges.push_back({ (char)255,(char)50, (char)50, ro, 0, totalUnits });
		}
	}

	std::list<MeterMarking> markings;
	markings.push_back({ (char)255, (char)255, (char)255, opacity, unitWidth, range - 80 });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, unitWidth, range + 80 });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, unitWidth, range - 160 });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, unitWidth, range + 160 });
	markings.push_back({ (char)0, (char)0, (char)0, 0.6, unitWidth*2, relLocation });

	RenderMeter(canvas, startPos, reqSize, baseColor, border, totalUnits, ranges, markings, false);

	//draw which kickoff spawn this is and its limit
	if (KickoffLike() && kickoffSpawn >= 0)
	{
		const KickoffSpawn& s = KICKOFF_SPAWNS[kickoffSpawn];
		string spawnMsg = fmt::format("{0} - speedflip limit {1:.2f}s", s.name, LimitForSpawn(s));
		canvas.SetColor(255, 255, 255, (char)(255 * opacity));
		canvas.SetPosition(Vector2{ startPos.X, (int)(startPos.Y - 20) });
		canvas.DrawString(spawnMsg, 1, 1, true, false);
	}

	//draw speed label
	auto speedCvar = _globalCvarManager->getCvar("sv_soccar_gamespeed");
	float speed = speedCvar.getFloatValue();
	string msg = fmt::format("Game speed: {0}%", (int)(speed * 100));
	int width = (msg.length() * 8.5) - 10;
	canvas.SetColor(255, 255, 255, (char)(255 * opacity));
	canvas.SetPosition(Vector2{ startPos.X + boxSize.X - width, (int)(startPos.Y - 20) });
	canvas.DrawString(msg, 1, 1, true, false);

	int ms = (int)(attempt.ticksNotPressingBoost / 120.0 * 1000.0);
	if (ms != 0)
	{
		canvas.SetColor(255, 255, 50, (char)(255 * opacity));
		//draw time not pressing boost label
		msg = fmt::format("Not pressing Boost: {0}ms", ms);
		width = 200;
		canvas.SetPosition(Vector2{ startPos.X, (int)(startPos.Y + boxSize.Y + 10) });
		canvas.DrawString(msg, 1, 1, true, false);
	}

	ms = (int)(attempt.ticksNotPressingThrottle / 120.0 * 1000.0);
	if (ms != 0)
	{
		canvas.SetColor(255, 255, 50, (char)(255 * opacity));
		//draw time not pressing throttle label
		msg = fmt::format("Not pressing Throttle: {0}ms", ms);
		width = 200;
		canvas.SetPosition(Vector2{ startPos.X, (int)(startPos.Y + boxSize.Y + 25) });
		canvas.DrawString(msg, 1, 1, true, false);
	}
}

void SpeedFlipTrainer::RenderFirstJumpMeter(CanvasWrapper canvas, float screenWidth, float screenHeight)
{
	int totalUnits = *jumpHigh - *jumpLow;
	int halfMark = totalUnits / 2;
	
	float opacity = 1.0;
	Vector2 reqSize = Vector2{ (int)(screenWidth * 2 / 100.f), (int)(screenHeight * 56 / 100.f) };
	int unitWidth = reqSize.Y / totalUnits;

	Vector2 boxSize = Vector2{ reqSize.X, unitWidth * totalUnits };
	Vector2 startPos = Vector2{ (int)((screenWidth * 75 / 100.f) + 2.5 * reqSize.X), (int)((screenHeight * 80 / 100.f) - boxSize.Y) };
	
	struct Color baseColor = { (char)255, (char)255, (char)255, opacity };
	struct Line border = { (char)255, (char)255, (char)255, opacity, 2 };

	
	std::list<MeterMarking> markings;
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, (int)(halfMark - 15) });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, (int)(halfMark - 10) });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, (int)(halfMark + 5) });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, (int)(halfMark + 10) });

	std::list<MeterRange> ranges;
	ranges.push_back({ (char)255, (char)255, (char)50, 0.2, (int)(halfMark - 15), (int)(halfMark - 10) });
	ranges.push_back({ (char)50,(char)255, (char)50, 0.2, (int)(halfMark - 10), (int)(halfMark + 5) });
	ranges.push_back({ (char)255,(char)255, (char)50, 0.2, (int)(halfMark + 5), (int)(halfMark + 10) });

	if (attempt.jumped)
	{
		int ticks = attempt.jumpTick - ((*jumpHigh - (halfMark)) - halfMark);
		if (ticks < 0)
		{
			ticks = 0;
		}
		else if (ticks > totalUnits)
		{
			ticks = totalUnits;
		}

		if (ticks < halfMark - 15)
		{
			ranges.push_back({ (char)255,(char)50, (char)50, 1, 0, halfMark - 15 });
		}
		else if (ticks < halfMark - 10)
		{
			ranges.push_back({ (char)255, (char)255, (char)50, 1, (int)(halfMark - 15), (int)(halfMark - 10) });
		}
		else if (ticks < halfMark + 5)
		{
			ranges.push_back({ (char)50,(char)255, (char)50, 1, (int)(halfMark - 10), (int)(halfMark + 5) });
		}
		else if (ticks < halfMark + 10)
		{
			ranges.push_back({ (char)255,(char)255, (char)50, 1, (int)(halfMark + 5), (int)(halfMark + 10) });
		}
		else
		{
			ranges.push_back({ (char)255,(char)50, (char)50, 1, (int)(halfMark + 10), totalUnits });
		}
		markings.push_back({ (char)0, (char)0, (char)0, 0.6, (int)reqSize.Y / totalUnits, (int)ticks });
	}

	RenderMeter(canvas, startPos, reqSize, baseColor, border, totalUnits, ranges, markings, true);

	//draw label
	string msg = "First Jump";
	canvas.SetColor(255, 255, 255, (char)(255 * opacity));
	canvas.SetPosition(Vector2{ (int)(startPos.X - 13), (int)(startPos.Y + boxSize.Y + 8) });
	canvas.DrawString(msg, 1, 1, true, false);

	auto ms = (int)(attempt.jumpTick * 1.0 / 120.0 * 1000.0 / 1.0);
	msg = to_string(ms) + " ms";
	canvas.SetPosition(Vector2{ startPos.X, (int)(startPos.Y + boxSize.Y + 8 + 15) });
	canvas.DrawString(msg, 1, 1, true, false);
}

void SpeedFlipTrainer::RenderFlipCancelMeter(CanvasWrapper canvas, float screenWidth, float screenHeight)
{
	float opacity = 1.0;
	int totalUnits = *flipCancelThreshold;

	Vector2 reqSize = Vector2{ (int)(screenWidth * 2 / 100.f), (int)(screenHeight * 55 / 100.f) };
	int unitWidth = reqSize.Y / totalUnits;

	Vector2 boxSize = Vector2{ reqSize.X, unitWidth * totalUnits };
	Vector2 startPos = Vector2{ (int)(screenWidth * 75 / 100.f), (int)((screenHeight * 80 / 100.f) - boxSize.Y) };

	struct Color baseColor = { (char)255, (char)255, (char)255, opacity };
	struct Line border = { (char)255, (char)255, (char)255, opacity, 2 };

	auto tickBeforeCancel = attempt.flipCancelTick - attempt.dodgedTick;

	// flip cancel time range
	std::list<MeterRange> ranges;
	if (attempt.flipCanceled)
	{
		auto ticks = tickBeforeCancel > totalUnits ? totalUnits : tickBeforeCancel;
		struct Color meterColor;
		if (ticks <= (int)(totalUnits * 0.6f))
			meterColor = { (char)50, (char)255, (char)50, 0.7 }; // green
		else if (ticks <= (int)(totalUnits * 0.9f))
			meterColor = { (char)255, (char)255, (char)50, 0.7 }; // yellow
		else
			meterColor = { (char)255, (char)50, (char)50, 0.7 }; // red
		ranges.push_back({ meterColor.red, meterColor.green, meterColor.blue, 1, 0, ticks });
	}

	std::list<MeterMarking> markings;
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, ((int)(totalUnits * 0.6f)) });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, ((int)(totalUnits * 0.9f)) });
	//markings.push_back({ (char)0, (char)0, (char)0, 0.6, 10, ticks });

	RenderMeter(canvas, startPos, reqSize, baseColor, border, totalUnits, ranges, markings, true);

	//draw label
	string msg = "Flip Cancel";
	canvas.SetColor(255, 255, 255, (char)(255 * opacity));
	canvas.SetPosition(Vector2{ (int)(startPos.X - 16), (int)(startPos.Y + boxSize.Y + 8) });
	canvas.DrawString(msg, 1, 1, true, false);

	auto ms = (int)(tickBeforeCancel * 1.0 / 120.0 * 1000.0 / 1.0);
	msg = to_string(ms) + " ms";
	canvas.SetPosition(Vector2{ startPos.X, (int)(startPos.Y + boxSize.Y + 8 + 15) });
	canvas.DrawString(msg, 1, 1, true, false);
}

void SpeedFlipTrainer::RenderAngleMeter(CanvasWrapper canvas, float screenWidth, float screenHeight)
{
	// Cap angle at 90
	int totalUnits = 180;
	
	float opacity = 1.0;
	Vector2 reqSize = Vector2{ (int)(screenWidth * 66 / 100.f), (int)(screenHeight * 4 / 100.f) };
	int unitWidth = reqSize.X / totalUnits;

	Vector2 boxSize = Vector2{ unitWidth * totalUnits, reqSize.Y };
	Vector2 startPos = Vector2{ (int)((screenWidth / 2) - (boxSize.X / 2)), (int)(screenHeight * 90 / 100.f) };

	struct Color baseColor = { (char)255, (char)255, (char)255, opacity };
	struct Line border = { (char)255, (char)255, (char)255, opacity, 2 };

	std::list<MeterRange> ranges;
	std::list<MeterMarking> markings;

	int greenRange = 8, yellowRange = 15;
	int lTarget = *optimalLeftAngle + 90;
	int rTarget = *optimalRightAngle + 90;

	int lyl = lTarget - yellowRange;
	int lgl = lTarget - greenRange;
	int lgh = lTarget + greenRange;
	int lyh = lTarget + yellowRange;

	int ryl = rTarget - yellowRange;
	int rgl = rTarget - greenRange;
	int rgh = rTarget + greenRange;
	int ryh = rTarget + yellowRange;

	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, lgh });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, lyh });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, lgl });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, lyl });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, rgh });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, ryh });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, rgl });
	markings.push_back({ (char)255, (char)255, (char)255, opacity, 3, ryl });

	ranges.push_back({ (char)255, (char)255, (char)50, 0.2, lyl, lgl });
	ranges.push_back({ (char)50, (char)255, (char)50, 0.2, lgl, lgh });
	ranges.push_back({ (char)255, (char)255, (char)50, 0.2, lgh, lyh });
	ranges.push_back({ (char)255, (char)255, (char)50, 0.2, ryl, rgl });
	ranges.push_back({ (char)50, (char)255, (char)50, 0.2, rgl, rgh });
	ranges.push_back({ (char)255, (char)255, (char)50, 0.2, rgh, ryh });

	if (attempt.dodged)
	{
		int angle = attempt.dodgeAngle;
		if (angle > 90) angle = 90;
		if (angle < -90) angle = -90;

		int angleAdjusted = 90 + angle;
		markings.push_back({ (char)0, (char)0, (char)0, 0.6, unitWidth, angleAdjusted });

		if (angleAdjusted < lyl)
		{
			ranges.push_back({ (char)255,(char)50, (char)50, 1, 0, lyl });
		}
		else if (angleAdjusted < lgl)
		{
			ranges.push_back({ (char)255, (char)255, (char)50, 1, lTarget - yellowRange, lTarget - greenRange });
		}
		else if (angleAdjusted < lgh)
		{
			ranges.push_back({ (char)50, (char)255, (char)50, 1, lTarget - greenRange, lTarget + greenRange });
		}
		else if (angleAdjusted < lyh)
		{
			ranges.push_back({ (char)255, (char)255, (char)50, 1, lTarget + greenRange, lTarget + yellowRange });
		}
		else if (angleAdjusted < ryl)
		{
			ranges.push_back({ (char)255,(char)50, (char)50, 1, lTarget + yellowRange, rTarget - yellowRange });
		}
		else if (angleAdjusted < rgl)
		{
			ranges.push_back({ (char)255, (char)255, (char)50, 1, rTarget - yellowRange, rTarget - greenRange });
		}
		else if (angleAdjusted < rgh)
		{
			ranges.push_back({ (char)50, (char)255, (char)50, 1, rTarget - greenRange, rTarget + greenRange });
		}
		else if (angleAdjusted < ryh)
		{
			ranges.push_back({ (char)255, (char)255, (char)50, 1, rTarget + greenRange, rTarget + yellowRange });
		}
		else
		{
			ranges.push_back({ (char)255, (char)50, (char)50, 1, rTarget + yellowRange, totalUnits });
		}
	}

	RenderMeter(canvas, startPos, reqSize, baseColor, border, totalUnits, ranges, markings, false);

	//draw angle label
	canvas.SetColor(255, 255, 255, (char)(255 * opacity));
	canvas.SetPosition(Vector2{ startPos.X, (int)(startPos.Y - 20) });
	canvas.DrawString("Dodge Angle: " + to_string(attempt.dodgeAngle) + " DEG", 1, 1, true, false);

	//draw time to ball label: green if you made it within the limit, red if not
	{
		string msg;
		bool good = false;
		if (attempt.reached && attempt.ticksToBall > 0)
		{
			auto secs = attempt.ticksToBall * 1.0 / 120.0;
			msg = fmt::format("Time to ball: {0:.4f}s (limit {1:.2f}s)", secs, timeLimit);
			good = attempt.passed;
		}
		else if (attempt.exploded || attempt.timedOut)
		{
			msg = fmt::format("Too slow - limit {0:.2f}s", timeLimit);
		}

		if (!msg.empty())
		{
			int width = (msg.length() * 8) - (5 * 3); // 8 pixels per char - 5 pixels per space

			if (good)
				canvas.SetColor(50, 255, 50, (char)(255 * opacity));
			else
				canvas.SetColor(255, 10, 10, (char)(255 * opacity));
			canvas.SetPosition(Vector2{ startPos.X + (int)(boxSize.X / 2) - (width/2), startPos.Y - 20 });
			canvas.DrawString(msg, 1, 1, true, false);
		}
	}

	string msg = fmt::format("Horizontal distance traveled: {0:.1f}", attempt.traveledY);
	int width = (msg.length() * 6.6);

	//draw angle label
	if(attempt.traveledY < 225)
		canvas.SetColor(50, 255, 50, (char)(255 * opacity));
	else if(attempt.traveledY < 425)
		canvas.SetColor(255, 255, 50, (char)(255 * opacity));
	else
		canvas.SetColor(255, 10, 10, (char)(255 * opacity));

	canvas.SetPosition(Vector2{ startPos.X + boxSize.X - width, (int)(startPos.Y - 20) });
	canvas.DrawString(msg, 1, 1, true, false);

	int start = 10;
	if (attempt.startedInAir)
	{
		msg = "Started before car touched the ground! Wait for car to settle on next attempt.";
		canvas.SetColor(255, 10, 10, (char)(255 * opacity));
		canvas.SetPosition(Vector2{ startPos.X + (boxSize.X / 2) - 265, (int)(startPos.Y + boxSize.Y + start) });
		canvas.DrawString(msg, 1, 1, true, false);
		start += 15;
	}
	if (attempt.startedNoBoost)
	{
		msg = "Was not pressing boost when time started! Press boost before you press throttle on next attempt.";
		canvas.SetColor(255, 10, 10, (char)(255 * opacity));
		canvas.SetPosition(Vector2{ startPos.X + (boxSize.X / 2) - 330, (int)(startPos.Y + boxSize.Y + start) });
		canvas.DrawString(msg, 1, 1, true, false);
	}
}

void SpeedFlipTrainer::PlayAttempt(Attempt* a, shared_ptr<GameWrapper> gameWrapper, ControllerInput* ci)
{
	if (a->inputs.size() <= 0)
		return;

	int currentPhysicsFrame = gameWrapper->GetEngine().GetPhysicsFrame();
	int tick = currentPhysicsFrame - startingPhysicsFrame;

	a->Play(ci, tick);

	gameWrapper->OverrideParams(ci, sizeof(ControllerInput));
}

void SpeedFlipTrainer::PlayBot(shared_ptr<GameWrapper> gameWrapper, ControllerInput* ci)
{
	int currentPhysicsFrame = gameWrapper->GetEngine().GetPhysicsFrame();
	int tick = currentPhysicsFrame - startingPhysicsFrame;

	bot.Play(ci, tick);

	gameWrapper->OverrideParams(ci, sizeof(ControllerInput));
}

// ---------------------------------------------------------------------------
// Stick circle: the dodge angle drawn on a joystick-style ring.
// 0 deg is straight up (forward), negative angles are left, positive are right,
// matching the angle used by the bar meter at the bottom of the screen.
// ---------------------------------------------------------------------------

namespace
{
	enum class AngleZone { Red, Yellow, Green };

	// Same thresholds as the bar meter: within 8 deg of a target is green,
	// within 15 deg is yellow, everything else is red.
	AngleZone ZoneForAngle(float angle, int leftTarget, int rightTarget)
	{
		const float greenRange = 8.0f, yellowRange = 15.0f;
		float d = (std::min)(fabsf(angle - leftTarget), fabsf(angle - rightTarget));
		if (d <= greenRange) return AngleZone::Green;
		if (d <= yellowRange) return AngleZone::Yellow;
		return AngleZone::Red;
	}

	// Filled triangles are drawn without transparency, so "dim" is done
	// with darker colors rather than alpha.
	void SetZoneColor(CanvasWrapper& canvas, AngleZone zone, bool bright)
	{
		switch (zone)
		{
		case AngleZone::Green:
			bright ? canvas.SetColor((char)60, (char)255, (char)60, (char)255) : canvas.SetColor((char)30, (char)95, (char)35, (char)255); break;
		case AngleZone::Yellow:
			bright ? canvas.SetColor((char)255, (char)235, (char)60, (char)255) : canvas.SetColor((char)100, (char)92, (char)30, (char)255); break;
		default:
			bright ? canvas.SetColor((char)255, (char)60, (char)60, (char)255) : canvas.SetColor((char)70, (char)32, (char)32, (char)255); break;
		}
	}

	// Point on the circle for a given angle (deg, clockwise from up) and radius
	Vector2F PointAt(Vector2F center, float angleDeg, float radius)
	{
		float rad = angleDeg * (float)M_PI / 180.0f;
		return Vector2F{ center.X + radius * sinf(rad), center.Y - radius * cosf(rad) };
	}

	void DrawCenteredString(CanvasWrapper& canvas, const std::string& text, Vector2F center, float scale)
	{
		Vector2F size = canvas.GetStringSize(text, scale, scale);
		canvas.SetPosition(Vector2F{ center.X - size.X / 2, center.Y - size.Y / 2 });
		canvas.DrawString(text, scale, scale, true, false);
	}

	void FillSquare(CanvasWrapper& canvas, Vector2F c, float half)
	{
		canvas.SetPosition(Vector2F{ c.X - half, c.Y - half });
		canvas.FillBox(Vector2F{ half * 2, half * 2 });
	}
}

void SpeedFlipTrainer::RenderStickCircle(CanvasWrapper canvas, float screenWidth, float screenHeight)
{
	const int leftTarget = *optimalLeftAngle;
	const int rightTarget = *optimalRightAngle;

	const float outerR = screenHeight * (*circleSize) / 100.f;
	const float innerR = outerR * 0.70f;
	const Vector2F center{ screenWidth * (*circleX) / 100.f, screenHeight * (*circleY) / 100.f };

	const bool haveDodge = attempt.dodged;
	const float dodge = (float)attempt.dodgeAngle;
	const AngleZone dodgeZone = ZoneForAngle(dodge, leftTarget, rightTarget);

	// Which stretch of the ring to light up: the zone the flip landed in
	// (only a small wedge for red, since red covers most of the ring)
	float bandLow = dodge, bandHigh = dodge;
	if (haveDodge)
	{
		const float maxSpread = dodgeZone == AngleZone::Red ? 12.0f : 360.0f;
		while (bandLow > dodge - maxSpread && ZoneForAngle(bandLow - 1, leftTarget, rightTarget) == dodgeZone) bandLow -= 1;
		while (bandHigh < dodge + maxSpread && ZoneForAngle(bandHigh + 1, leftTarget, rightTarget) == dodgeZone) bandHigh += 1;
	}

	// Colored ring, one degree per segment so the zone edges line up exactly.
	// Muted colors until a flip lights up its zone.
	for (int deg = -180; deg < 180; deg++)
	{
		float a0 = (float)deg, a1 = (float)(deg + 1), mid = deg + 0.5f;
		AngleZone zone = ZoneForAngle(mid, leftTarget, rightTarget);
		bool lit = haveDodge && zone == dodgeZone && mid >= bandLow - 0.5f && mid <= bandHigh + 0.5f;
		SetZoneColor(canvas, zone, lit);

		Vector2F o0 = PointAt(center, a0, outerR), o1 = PointAt(center, a1, outerR);
		Vector2F i0 = PointAt(center, a0, innerR), i1 = PointAt(center, a1, innerR);
		canvas.FillTriangle(o0, o1, i1);
		canvas.FillTriangle(o0, i1, i0);
	}

	// Ring outlines and tick marks at each zone edge (like the bar meter)
	canvas.SetColor((char)255, (char)255, (char)255, (char)170);
	for (int deg = -180; deg < 180; deg += 3)
	{
		canvas.DrawLine(PointAt(center, (float)deg, outerR), PointAt(center, (float)(deg + 3), outerR), 2);
		canvas.DrawLine(PointAt(center, (float)deg, innerR), PointAt(center, (float)(deg + 3), innerR), 1);
	}
	for (int target : { leftTarget, rightTarget })
		for (int edge : { -15, -8, 8, 15 })
			canvas.DrawLine(PointAt(center, (float)(target + edge), innerR), PointAt(center, (float)(target + edge), outerR), 2);

	// Faint forward guide and labels
	canvas.SetColor((char)255, (char)255, (char)255, (char)90);
	canvas.DrawLine(PointAt(center, 0, 4), PointAt(center, 0, innerR), 1);
	canvas.SetColor((char)255, (char)255, (char)255, (char)230);
	DrawCenteredString(canvas, "FWD", PointAt(center, 0, outerR + 14), 1.0f);
	DrawCenteredString(canvas, "L", PointAt(center, -90, outerR + 14), 1.0f);
	DrawCenteredString(canvas, "R", PointAt(center, 90, outerR + 14), 1.0f);

	if (!haveDodge)
	{
		// Before the flip: live stick dot so you can line up
		float sx = liveInput.Steer, sy = -liveInput.Pitch;
		float mag = sqrtf(sx * sx + sy * sy);
		if (mag > 0.1f)
		{
			float liveAngle = atan2f(sx, sy) * 180.0f / (float)M_PI;
			canvas.SetColor((char)255, (char)255, (char)255, (char)200);
			FillSquare(canvas, PointAt(center, liveAngle, innerR * (std::min)(mag, 1.0f)), 4);
		}
		canvas.SetColor((char)255, (char)255, (char)255, (char)200);
		DrawCenteredString(canvas, "FLIP -- DEG", Vector2F{ center.X, center.Y + outerR + 26 }, 1.5f);
		return;
	}

	// FLIP line: the direction the car actually flipped (thick, zone colored)
	{
		Vector2F from = PointAt(center, dodge, 4);
		Vector2F to = PointAt(center, dodge, outerR + 10);
		canvas.SetColor((char)0, (char)0, (char)0, (char)230);
		canvas.DrawLine(from, to, 8);
		SetZoneColor(canvas, dodgeZone, true);
		canvas.DrawLine(from, to, 4);
	}

	// STICK marker: where the stick was on the frame you pressed the second jump
	// (thin white line ending in a hollow square at how far the stick was pushed)
	if (attempt.stickRecorded)
	{
		float mag = (std::min)(sqrtf(attempt.stickX * attempt.stickX + attempt.stickY * attempt.stickY), 1.0f);
		float sa = (float)attempt.stickAngle;
		Vector2F tip = PointAt(center, sa, innerR * mag);
		canvas.SetColor((char)0, (char)0, (char)0, (char)200);
		canvas.DrawLine(PointAt(center, sa, 4), tip, 4);
		canvas.SetColor((char)255, (char)255, (char)255, (char)255);
		canvas.DrawLine(PointAt(center, sa, 4), tip, 2);
		canvas.SetPosition(Vector2F{ tip.X - 6, tip.Y - 6 });
		canvas.DrawBox(Vector2F{ 12, 12 });
		// Tick on the ring at the stick angle too
		canvas.DrawLine(PointAt(center, sa, innerR - 2), PointAt(center, sa, outerR + 2), 2);
	}

	// Readouts under the circle
	SetZoneColor(canvas, dodgeZone, true);
	DrawCenteredString(canvas, "FLIP " + std::to_string(attempt.dodgeAngle) + " DEG", Vector2F{ center.X, center.Y + outerR + 26 }, 1.5f);
	canvas.SetColor((char)255, (char)255, (char)255, (char)230);
	std::string stickText = attempt.stickRecorded ? ("STICK " + std::to_string(attempt.stickAngle) + " DEG") : "STICK --";
	DrawCenteredString(canvas, stickText, Vector2F{ center.X, center.Y + outerR + 48 }, 1.2f);
}
