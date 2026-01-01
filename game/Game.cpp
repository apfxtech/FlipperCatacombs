#include "game/Defines.h"
#include "game/Game.h"
#include "game/FixedMath.h"
#include "game/Draw.h"
#include "game/Map.h"
#include "game/Projectile.h"
#include "game/Particle.h"
#include "game/MapGenerator.h"
#include "game/Platform.h"
#include "game/Entity.h"
#include "game/Enemy.h"
#include "game/Menu.h"
#include "game/Font.h"

Player Game::players[2];
Player& Game::player = Game::players[0];

const char* Game::displayMessage = nullptr;
uint8_t Game::displayMessageTime = 0;
Game::State Game::state = Game::State::Menu;
uint8_t Game::floor = 1;
uint8_t Game::globalTickFrame = 0;
Stats Game::stats;
Menu Game::menu;
uint8_t Game::localPlayerId = 0;
char Game::localNetworkToken = 0;
bool Game::onlineEnabled = false;

bool Game::IsOnlineEnabled() {
    return onlineEnabled;
}

Player& Game::GetLocalPlayer() {
    return players[localPlayerId];
}

Player& Game::GetRemotePlayer() {
    return players[!localPlayerId];
}

void Game::InitOffline() {
    menu.Init();
    ParticleSystemManager::Init();
    ProjectileManager::Init();
    EnemyManager::Init();
}

void Game::InitOnline() {
    state = State::EstablishingNetwork;
    localNetworkToken = PlatformNet::GenerateRandomNetworkToken();

    menu.Init();
    ParticleSystemManager::Init();
    ProjectileManager::Init();
    EnemyManager::Init();
}

void Game::Init() {
    onlineEnabled = false;
    state = State::Menu;
    localPlayerId = 0;
    localNetworkToken = 0;
    InitOffline();
}

bool Game::IsInMenu() {
    return (state != State::InGame) && (state != State::FadeOut);
}

void Game::GoToMenu() {
    Game::stats.killedBy = EnemyType::Exit;
    SwitchState(State::FadeOut);
}

void Game::StartGame() {
    StartGameStory();
}

void Game::StartGameStory() {
    onlineEnabled = false;

    floor = 1;
    stats.Reset();
    players[0].Init();
    SwitchState(State::EnteringLevel);
}

void Game::StartGameServer() {
    onlineEnabled = true;

    state = State::EstablishingNetwork;
    localNetworkToken = PlatformNet::GenerateRandomNetworkToken();

    menu.ResetTimer();

    globalTickFrame = 0;
    floor = 1;
    stats.Reset();
    players[0].Init();
    players[1].Init();
}

void Game::SwitchState(State newState) {
    if(state != newState) {
        state = newState;
        menu.ResetTimer();
    }
}

void Game::ShowMessage(const char* message) {
    constexpr uint8_t messageDisplayTime = 90;
    displayMessage = message;
    displayMessageTime = messageDisplayTime;
}

void Game::NextLevel() {
    if(floor == 10) {
        GameOver();
    } else {
        floor++;
        SwitchState(State::EnteringLevel);
    }
}

void Game::Respawn() {
}

void Game::StartLevel() {
    ParticleSystemManager::Init();
    ProjectileManager::Init();
    EnemyManager::Init();
    MapGenerator::Generate();
    EnemyManager::SpawnEnemies();

    players[0].NextLevel();
    if(onlineEnabled) players[1].NextLevel();

    Platform::ExpectLoadDelay();
    SwitchState(State::InGame);
}

void Game::DrawOffline() {
    switch(state) {
        case State::Menu:
            menu.Draw();
            break;
        case State::EnteringLevel:
            menu.DrawEnteringLevel();
            break;
        case State::InGame: {
            Renderer::camera.x = player.x;
            Renderer::camera.y = player.y;
            Renderer::camera.angle = player.angle;
            Renderer::Render();
        } break;
        case State::GameOver:
            menu.DrawGameOver();
            break;
        case State::FadeOut:
            menu.FadeOut();
            break;
        default:
            break;
    }
}

void Game::DrawOnline() {
    switch(state) {
        case State::EstablishingNetwork:
        case State::SendSyncMessage:
        case State::RecvSyncMessage:
            menu.DrawEstablishingNetwork();
            break;
        case State::Menu:
            menu.Draw();
            break;
        case State::EnteringLevel:
            menu.DrawEnteringLevel();
            break;
        case State::InGame: {
            Player& p = GetLocalPlayer();
            Renderer::camera.x = p.x;
            Renderer::camera.y = p.y;
            Renderer::camera.angle = p.angle;
            Renderer::Render();
        } break;
        case State::GameOver:
            menu.DrawGameOver();
            break;
        case State::FadeOut:
            menu.FadeOut();
            break;
        default:
            break;
    }
}

void Game::Draw() {
    if(onlineEnabled) DrawOnline();
    else DrawOffline();
}

void Game::TickInGameOffline() {
    if(displayMessageTime > 0) {
        displayMessageTime--;
        if(displayMessageTime == 0) displayMessage = nullptr;
    }

    player.Tick();

    ProjectileManager::Update();
    ParticleSystemManager::Update();
    EnemyManager::Update();

    if(Map::GetCellSafe(player.x / CELL_SIZE, player.y / CELL_SIZE) == CellType::Exit) {
        NextLevel();
    }

    if(player.hp == 0) {
        GameOver();
    }
}

bool Game::TickInGameOnline() {
    static bool waitingForRead = false;
    static uint8_t localInput = 0;

    if (!PlatformNet::IsAvailableForWrite()) {
        return false;
    }

    if (!waitingForRead) {
        localInput = Platform::GetInput();
        PlatformNet::Write(localInput);
    }

    uint8_t remoteInput;

    if (PlatformNet::IsAvailable()) {
        remoteInput = PlatformNet::Read();
        waitingForRead = false;
    } else {
        waitingForRead = true;
        return false;
    }

    if (displayMessageTime > 0) {
        displayMessageTime--;
        if (displayMessageTime == 0) displayMessage = nullptr;
    }

    ProjectileManager::Update();
    ParticleSystemManager::Update();
    EnemyManager::Update();

    for (int n = 0; n < 2; n++) {
        Player& p = players[n];
        p.Tick(localPlayerId == (uint8_t)n ? localInput : remoteInput);

        if (p.hp == 0) {
            GameOver();
            break;
        }

        if (Map::GetCell(p.x / CELL_SIZE, p.y / CELL_SIZE) == CellType::Exit) {
            NextLevel();
            break;
        }
    }

    return true;
}

void Game::SyncNetwork() {
    constexpr uint8_t syncCode = 0;

    if (state == State::SendSyncMessage && PlatformNet::IsAvailableForWrite()) {
        PlatformNet::Write(syncCode);
        state = State::RecvSyncMessage;
    }

    if (state == State::RecvSyncMessage && PlatformNet::IsAvailable()) {
        if (PlatformNet::Read() == syncCode) {
            globalTickFrame = 0;
            floor = 1;
            stats.Reset();
            players[0].Init();
            players[1].Init();
            SwitchState(State::EnteringLevel);
        }
    }
}

void Game::ConnectToNetwork() {
    if (PlatformNet::IsAvailableForWrite()) {
        static char pingTimer = 0;

        if (pingTimer == 0) {
            PlatformNet::Write(localNetworkToken);
            pingTimer = 30;
        } else pingTimer--;
    }

    if (PlatformNet::IsAvailable()) {
        char remoteToken = PlatformNet::Read();

        while (PlatformNet::IsAvailable()) {
            if (PlatformNet::Peek() == remoteToken) {
                PlatformNet::Read();
            }
        }

        if (remoteToken == localNetworkToken) {
            localNetworkToken = PlatformNet::GenerateRandomNetworkToken();
        } else {
            localPlayerId = localNetworkToken < remoteToken ? 0 : 1;
            SwitchState(State::SendSyncMessage);
        }
    }
}

bool Game::TickOffline() {
    globalTickFrame++;

    switch(state) {
        case State::InGame:
            TickInGameOffline();
            return true;
        case State::EnteringLevel:
            menu.TickEnteringLevel();
            return true;
        case State::Menu:
            menu.Tick();
            return true;
        case State::GameOver:
            menu.TickGameOver();
            return true;
        default:
            return true;
    }
}

bool Game::TickOnline() {
    bool success = true;

    switch(state) {
        case State::EstablishingNetwork:
            ConnectToNetwork();
            break;
        case State::SendSyncMessage:
        case State::RecvSyncMessage:
            SyncNetwork();
            break;
        case State::InGame:
        case State::FadeOut:
            success = TickInGameOnline();
            break;
        case State::EnteringLevel:
            menu.TickEnteringLevel();
            break;
        case State::Menu:
            menu.Tick();
            break;
        case State::GameOver:
            menu.TickGameOver();
            break;
        default:
            break;
    }

    if (success) {
        globalTickFrame++;
    }
    return success;
}

bool Game::Tick() {
    if(onlineEnabled) return TickOnline();
    return TickOffline();
}

void Game::GameOver() {
    SwitchState(State::FadeOut);
}

void Stats::Reset() {
    killedBy = EnemyType::None;
    chestsOpened = 0;
    coinsCollected = 0;
    crownsCollected = 0;
    scrollsCollected = 0;

    for(uint8_t& killCounter : enemyKills) {
        killCounter = 0;
    }
}
