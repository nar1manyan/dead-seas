#pragma once
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <SFML/Audio.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include "../../shared/include/protocol.hpp"

struct InterpEntity {
    float prevX{0.f}, prevY{0.f};
    float currX{0.f}, currY{0.f};
    float alpha{1.f};
    void setTarget(float x, float y) {
        prevX = currX;
        prevY = currY;
        currX = x;
        currY = y;
    }
    float ix() const { return prevX + (currX - prevX) * alpha; }
    float iy() const { return prevY + (currY - prevY) * alpha; }
};

struct Notification {
    sf::Text text;
    float lifetime{1.5f};
    float velY{-40.f};
};

class Renderer {
public:
    Renderer() = default;

    bool init(unsigned width, unsigned height, const std::string &title) {
        window_.create(sf::VideoMode(width, height), title,
                       sf::Style::Titlebar | sf::Style::Close);
        window_.setVerticalSyncEnabled(true);

        if (!font_.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf") &&
            !font_.loadFromFile("/usr/share/fonts/truetype/freefont/FreeSansBold.ttf") &&
            !font_.loadFromFile("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf")) {
        }

        heroTexture_.setSmooth(true);
        useHeroSprite_ = heroTexture_.loadFromFile("assets/rsz_hero.png");
        if (useHeroSprite_) {
            heroSprite_.setTexture(heroTexture_);
            auto ts = heroTexture_.getSize();
            heroSprite_.setOrigin(ts.x * 0.5f, ts.y * 0.5f);
            float scl = (Cfg::PLAYER_RADIUS * 2.f) / static_cast<float>(std::max(ts.x, ts.y));
            heroSprite_.setScale(scl, scl);
        }

        monsterTexture_.setSmooth(true);
        useMonsterSprite_ = monsterTexture_.loadFromFile("assets/rsz_monster.png");
        if (useMonsterSprite_) {
            monsterSprite_.setTexture(monsterTexture_);
            auto ts = monsterTexture_.getSize();
            monsterSprite_.setOrigin(ts.x * 0.5f, ts.y * 0.5f);
            float scl = (Cfg::ZOMBIE_RADIUS * 2.f) / static_cast<float>(std::max(ts.x, ts.y));
            monsterSprite_.setScale(scl, scl);
        }

        if (music_.openFromFile("assets/pirates.mp3")) {
            music_.setLoop(true);
            music_.setVolume(5.f);
            music_.play();
        }

        buildShapes();
        buildBackground(width, height);

        scoreTxt_.setFont(font_);
        scoreTxt_.setCharacterSize(20);
        scoreTxt_.setFillColor(sf::Color::White);
        scoreTxt_.setPosition(Cfg::WORLD_W - 160.f, 8.f);

        waveTxt_.setFont(font_);
        waveTxt_.setCharacterSize(20);
        waveTxt_.setFillColor(sf::Color(255, 200, 80));
        waveTxt_.setPosition(Cfg::WORLD_W * 0.5f - 40.f, 8.f);

        gameOverTxt_.setFont(font_);
        gameOverTxt_.setString("GAME OVER\nRespawning...");
        gameOverTxt_.setCharacterSize(48);
        gameOverTxt_.setFillColor(sf::Color(220, 60, 60));
        gameOverTxt_.setOutlineColor(sf::Color::Black);
        gameOverTxt_.setOutlineThickness(3.f);
        auto bounds = gameOverTxt_.getLocalBounds();
        gameOverTxt_.setOrigin(bounds.width / 2.f, bounds.height / 2.f);
        gameOverTxt_.setPosition(Cfg::WORLD_W / 2.f, Cfg::WORLD_H / 2.f);

        return true;
    }

    bool isOpen() const { return window_.isOpen(); }
    sf::RenderWindow &window() { return window_; }

    void setInterpolationAlpha(float a) {
        playerInterp_.alpha = a;
        for (auto &[id, e]: zombieInterps_) e.alpha = a;
    }

    void updateFromState(const StatePayload &s) {
        playerInterp_.setTarget(s.playerPos.x, s.playerPos.y);
        playerLives_ = s.playerLives;
        playerAlive_ = s.playerAlive != 0;
        score_ = s.score;
        wave_ = s.wave;

        for (uint8_t i = 0; i < s.zombieCount; ++i) {
            const auto &z = s.zombies[i];
            if (!z.alive) continue;
            zombieInterps_[z.id].setTarget(z.pos.x, z.pos.y);
            zombieHp_[z.id] = z.hp;
        }
        for (auto it = zombieInterps_.begin(); it != zombieInterps_.end();) {
            bool found = false;
            for (uint8_t i = 0; i < s.zombieCount; ++i)
                if (s.zombies[i].id == it->first && s.zombies[i].alive) {
                    found = true; break;
                }
            it = found ? ++it : zombieInterps_.erase(it);
        }
    }

    void addNotification(const std::string &msg, sf::Color col = sf::Color::White,
                         float x = 400.f, float y = 300.f) {
        Notification n;
        n.text.setFont(font_);
        n.text.setString(msg);
        n.text.setCharacterSize(22);
        n.text.setFillColor(col);
        n.text.setOutlineColor(sf::Color::Black);
        n.text.setOutlineThickness(1.5f);
        n.text.setPosition(x, y);
        notifications_.push_back(std::move(n));
    }

    void render(float dt) {
        window_.clear(sf::Color(30, 20, 40));
        window_.draw(backgroundSprite_);
        drawZombies();
        drawPlayer();
        drawHUD();
        updateAndDrawNotifications(dt);
        if (!playerAlive_) drawGameOver();
        window_.display();
    }

    uint8_t collectInput(float &outAimX, float &outAimY) const {
        uint8_t flags = 0;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::W) ||
            sf::Keyboard::isKeyPressed(sf::Keyboard::Up))
            flags |= InputFlag::MOVE_UP;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::S) ||
            sf::Keyboard::isKeyPressed(sf::Keyboard::Down))
            flags |= InputFlag::MOVE_DOWN;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::A) ||
            sf::Keyboard::isKeyPressed(sf::Keyboard::Left))
            flags |= InputFlag::MOVE_LEFT;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::D) ||
            sf::Keyboard::isKeyPressed(sf::Keyboard::Right))
            flags |= InputFlag::MOVE_RIGHT;
        if (sf::Mouse::isButtonPressed(sf::Mouse::Left)) flags |= InputFlag::ATTACK;

        auto mp = sf::Mouse::getPosition(window_);
        outAimX = static_cast<float>(mp.x);
        outAimY = static_cast<float>(mp.y);
        return flags;
    }

private:
    void buildBackground(unsigned w, unsigned h) {
        bgTexture_.create(w, h);
        bgTexture_.clear(sf::Color(20, 15, 30));

        sf::RectangleShape line;
        line.setFillColor(sf::Color(50, 40, 60, 180));
        for (float x = 0; x <= static_cast<float>(w); x += 64.f) {
            line.setSize({1.f, static_cast<float>(h)});
            line.setPosition(x, 0);
            bgTexture_.draw(line);
        }
        for (float y = 0; y <= static_cast<float>(h); y += 64.f) {
            line.setSize({static_cast<float>(w), 1.f});
            line.setPosition(0, y);
            bgTexture_.draw(line);
        }
        bgTexture_.display();
        backgroundSprite_.setTexture(bgTexture_.getTexture());
    }

    void buildShapes() {
        playerShape_.setRadius(Cfg::PLAYER_RADIUS);
        playerShape_.setFillColor(sf::Color(80, 200, 255));
        playerShape_.setOutlineColor(sf::Color::White);
        playerShape_.setOutlineThickness(2.f);
        playerShape_.setOrigin(Cfg::PLAYER_RADIUS, Cfg::PLAYER_RADIUS);

        zombieShape_.setSize({Cfg::ZOMBIE_RADIUS * 2.f, Cfg::ZOMBIE_RADIUS * 2.f});
        zombieShape_.setFillColor(sf::Color(60, 180, 60));
        zombieShape_.setOutlineColor(sf::Color(20, 100, 20));
        zombieShape_.setOutlineThickness(1.5f);
        zombieShape_.setOrigin(Cfg::ZOMBIE_RADIUS, Cfg::ZOMBIE_RADIUS);

        swordLine_[0] = sf::Vertex(sf::Vector2f(0, 0), sf::Color(255, 220, 50, 180));
        swordLine_[1] = sf::Vertex(sf::Vector2f(0, 0), sf::Color(255, 220, 50, 180));
    }

    void drawPlayer() {
        if (!playerAlive_) return;
        float ix = playerInterp_.ix();
        float iy = playerInterp_.iy();

        auto mp   = sf::Mouse::getPosition(window_);
        float dx  = static_cast<float>(mp.x) - ix;
        float dy  = static_cast<float>(mp.y) - iy;
        float len = std::sqrt(dx * dx + dy * dy);
        float angle = 0.f;
        if (len > 0.001f) {
            dx /= len; dy /= len;
            angle = std::atan2(dy, dx) * 180.f / 3.14159f + 90.f;
        }

        if (useHeroSprite_) {
            heroSprite_.setPosition(ix, iy);
            heroSprite_.setRotation(angle);
            window_.draw(heroSprite_);
        } else {
            playerShape_.setPosition(ix, iy);
            window_.draw(playerShape_);
            sf::RectangleShape hat({26.f, 10.f});
            hat.setFillColor(sf::Color(120, 80, 30));
            hat.setOrigin(13.f, 5.f);
            hat.setPosition(ix, iy - Cfg::PLAYER_RADIUS - 2.f);
            window_.draw(hat);
        }

        swordLine_[0].position = {ix, iy};
        swordLine_[1].position = {ix + dx * Cfg::ATTACK_RANGE, iy + dy * Cfg::ATTACK_RANGE};
        swordLine_[0].color = sf::Color(255, 220, 50, 120);
        swordLine_[1].color = sf::Color(255, 220, 50, 0);
        window_.draw(swordLine_, 2, sf::Lines);
    }

    void drawZombies() {
        for (auto &[id, e]: zombieInterps_) {
            float ex = e.ix(), ey = e.iy();

            if (useMonsterSprite_) {
                float dx = playerInterp_.ix() - ex;
                float dy = playerInterp_.iy() - ey;
                float angle = std::atan2(dy, dx) * 180.f / 3.14159f + 90.f;

                int hp = zombieHp_.count(id) ? zombieHp_[id] : 1;
                monsterSprite_.setColor(hp > 1 ? sf::Color::White : sf::Color(255, 100, 100));
                monsterSprite_.setPosition(ex, ey);
                monsterSprite_.setRotation(angle);
                window_.draw(monsterSprite_);
            } else {
                int hp = zombieHp_.count(id) ? zombieHp_[id] : 1;
                sf::Color col = hp > 1 ? sf::Color(60, 180, 60) : sf::Color(200, 80, 80);
                zombieShape_.setFillColor(col);
                zombieShape_.setPosition(ex, ey);
                window_.draw(zombieShape_);

                sf::CircleShape eye(3.f);
                eye.setFillColor(sf::Color::Red);
                eye.setOrigin(3.f, 3.f);
                eye.setPosition(ex - 5.f, ey - 4.f);
                window_.draw(eye);
                eye.setPosition(ex + 5.f, ey - 4.f);
                window_.draw(eye);
            }
        }
    }

    void drawHUD() {
        float hx = 12.f;
        for (int i = 0; i < 5; ++i) {
            sf::CircleShape heart(10.f);
            heart.setOrigin(10.f, 10.f);
            heart.setPosition(hx + i * 26.f, 20.f);
            heart.setFillColor(i < playerLives_ ? sf::Color(220, 60, 60) : sf::Color(60, 40, 40));
            heart.setOutlineColor(sf::Color::White);
            heart.setOutlineThickness(1.f);
            window_.draw(heart);
        }

        scoreTxt_.setString("Score: " + std::to_string(score_));
        window_.draw(scoreTxt_);

        waveTxt_.setString("Wave: " + std::to_string(wave_));
        window_.draw(waveTxt_);
    }

    void drawGameOver() {
        sf::RectangleShape overlay({Cfg::WORLD_W, Cfg::WORLD_H});
        overlay.setFillColor(sf::Color(0, 0, 0, 160));
        window_.draw(overlay);
        window_.draw(gameOverTxt_);
    }

    void updateAndDrawNotifications(float dt) {
        for (auto it = notifications_.begin(); it != notifications_.end();) {
            it->lifetime -= dt;
            if (it->lifetime <= 0.f) { it = notifications_.erase(it); continue; }
            auto pos = it->text.getPosition();
            it->text.setPosition(pos.x, pos.y + it->velY * dt);
            float alpha = std::min(1.f, it->lifetime / 0.5f) * 255.f;
            auto col = it->text.getFillColor();
            col.a = static_cast<uint8_t>(alpha);
            it->text.setFillColor(col);
            window_.draw(it->text);
            ++it;
        }
    }

    sf::RenderWindow window_;
    sf::Font font_;

    sf::RenderTexture bgTexture_;
    sf::Sprite backgroundSprite_;

    sf::Texture heroTexture_;
    sf::Sprite  heroSprite_;
    bool useHeroSprite_{false};

    sf::Texture monsterTexture_;
    sf::Sprite  monsterSprite_;
    bool useMonsterSprite_{false};

    sf::Music music_;

    sf::CircleShape    playerShape_;
    sf::RectangleShape zombieShape_;
    sf::Vertex         swordLine_[2];

    sf::Text scoreTxt_;
    sf::Text waveTxt_;
    sf::Text gameOverTxt_;

    InterpEntity playerInterp_;
    std::unordered_map<uint32_t, InterpEntity> zombieInterps_;
    std::unordered_map<uint32_t, int> zombieHp_;
    int playerLives_{3};
    bool playerAlive_{true};
    uint32_t score_{0};
    uint32_t wave_{1};
    std::vector<Notification> notifications_;
};
