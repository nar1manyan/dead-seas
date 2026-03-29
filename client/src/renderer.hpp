#pragma once
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
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
        window_.setFramerateLimit(60);
        if (!font_.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf") &&
            !font_.loadFromFile("/usr/share/fonts/truetype/freefont/FreeSansBold.ttf") &&
            !font_.loadFromFile("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf")) {
        }
        buildShapes();
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

        //zombie interactions
        for (uint8_t i = 0; i < s.zombieCount; ++i) {
            const auto &z = s.zombies[i];
            if (!z.alive) continue;
            zombieInterps_[z.id].setTarget(z.pos.x, z.pos.y);
            zombieHp_[z.id] = z.hp;
        }
        //dead zombie cleaner
        for (auto it = zombieInterps_.begin(); it != zombieInterps_.end();) {
            bool found = false;
            for (uint8_t i = 0; i < s.zombieCount; ++i)
                if (s.zombies[i].id == it->first && s.zombies[i].alive) {
                    found = true;
                    break;
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
        drawGrid();
        drawZombies();
        drawPlayer();
        drawHUD();
        updateAndDrawNotifications(dt);

        if (!playerAlive_) drawGameOver();
        window_.display();
    }

    //user input
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
    void buildShapes() {
        // #TODO change player visual, zombie visual, change weapon

        //player
        playerShape_.setRadius(Cfg::PLAYER_RADIUS);
        playerShape_.setFillColor(sf::Color(80, 200, 255));
        playerShape_.setOutlineColor(sf::Color::White);
        playerShape_.setOutlineThickness(2.f);
        playerShape_.setOrigin(Cfg::PLAYER_RADIUS, Cfg::PLAYER_RADIUS);

        //zombie
        zombieShape_.setSize({Cfg::ZOMBIE_RADIUS * 2.f, Cfg::ZOMBIE_RADIUS * 2.f});
        zombieShape_.setFillColor(sf::Color(60, 180, 60));
        zombieShape_.setOutlineColor(sf::Color(20, 100, 20));
        zombieShape_.setOutlineThickness(1.5f);
        zombieShape_.setOrigin(Cfg::ZOMBIE_RADIUS, Cfg::ZOMBIE_RADIUS);

        //weapon
        swordLine_[0] = sf::Vertex(sf::Vector2f(0, 0), sf::Color(255, 220, 50, 180));
        swordLine_[1] = sf::Vertex(sf::Vector2f(0, 0), sf::Color(255, 220, 50, 180));
    }

    void drawGrid() {
        //background
        // #TODO Change background from grid to image
        sf::RectangleShape line;
        line.setFillColor(sf::Color(50, 40, 60));
        for (float x = 0; x <= Cfg::WORLD_W; x += 64.f) {
            line.setSize({1.f, Cfg::WORLD_H});
            line.setPosition(x, 0);
            window_.draw(line);
        }
        for (float y = 0; y <= Cfg::WORLD_H; y += 64.f) {
            line.setSize({Cfg::WORLD_W, 1.f});
            line.setPosition(0, y);
            window_.draw(line);
        }
    }

    void drawPlayer() {
        if (!playerAlive_) return;
        float ix = playerInterp_.ix();
        float iy = playerInterp_.iy();

        playerShape_.setPosition(ix, iy);
        window_.draw(playerShape_);

        //hat
        sf::RectangleShape hat({26.f, 10.f});
        hat.setFillColor(sf::Color(120, 80, 30));
        hat.setOrigin(13.f, 5.f);
        hat.setPosition(ix, iy - Cfg::PLAYER_RADIUS - 2.f);
        window_.draw(hat);

        //weapon
        auto mp = sf::Mouse::getPosition(window_);
        float dx = mp.x - ix, dy = mp.y - iy;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.001f) {
            dx /= len;
            dy /= len;
        }

        swordLine_[0].position = {ix, iy};
        swordLine_[1].position = {
            ix + dx * Cfg::ATTACK_RANGE,
            iy + dy * Cfg::ATTACK_RANGE
        };
        swordLine_[0].color = sf::Color(255, 220, 50, 100);
        swordLine_[1].color = sf::Color(255, 220, 50, 0);
        window_.draw(swordLine_, 2, sf::Lines);
    }

    void drawZombies() {
        for (auto &[id, e]: zombieInterps_) {
            zombieShape_.setPosition(e.ix(), e.iy());

            //zombie hp || change color
            int hp = zombieHp_.count(id) ? zombieHp_[id] : 1;
            sf::Color col = hp > 1 ? sf::Color(60, 180, 60) : sf::Color(200, 80, 80);
            zombieShape_.setFillColor(col);
            window_.draw(zombieShape_);

            // zombie eye
            sf::CircleShape eye(3.f);
            eye.setFillColor(sf::Color::Red);
            eye.setOrigin(3.f, 3.f);
            eye.setPosition(e.ix() - 5.f, e.iy() - 4.f);
            window_.draw(eye);
            eye.setPosition(e.ix() + 5.f, e.iy() - 4.f);
            window_.draw(eye);
        }
    }

    void drawHUD() {
        //hearts
        // #TODO Change heart from pixel circle to image
        float hx = 12.f;
        for (int i = 0; i < 5; ++i) {
            sf::CircleShape heart(10.f);
            heart.setOrigin(10.f, 10.f);
            heart.setPosition(hx + i * 26.f, 20.f);
            if (i < playerLives_)
                heart.setFillColor(sf::Color(220, 60, 60));
            else
                heart.setFillColor(sf::Color(60, 40, 40));
            heart.setOutlineColor(sf::Color::White);
            heart.setOutlineThickness(1.f);
            window_.draw(heart);
        }

        //score
        sf::Text scoreTxt;
        scoreTxt.setFont(font_);
        scoreTxt.setString("Score: " + std::to_string(score_));
        scoreTxt.setCharacterSize(20);
        scoreTxt.setFillColor(sf::Color::White);
        scoreTxt.setPosition(Cfg::WORLD_W - 160.f, 8.f);
        window_.draw(scoreTxt);

        //waves
        sf::Text waveTxt;
        waveTxt.setFont(font_);
        waveTxt.setString("Wave: " + std::to_string(wave_));
        waveTxt.setCharacterSize(20);
        waveTxt.setFillColor(sf::Color(255, 200, 80));
        waveTxt.setPosition(Cfg::WORLD_W * 0.5f - 40.f, 8.f);
        window_.draw(waveTxt);
    }

    void drawGameOver() {
        sf::RectangleShape overlay({Cfg::WORLD_W, Cfg::WORLD_H});
        overlay.setFillColor(sf::Color(0, 0, 0, 160));
        window_.draw(overlay);

        sf::Text t;
        t.setFont(font_);
        t.setString("GAME OVER\nRespawning...");
        t.setCharacterSize(48);
        t.setFillColor(sf::Color(220, 60, 60));
        t.setOutlineColor(sf::Color::Black);
        t.setOutlineThickness(3.f);
        auto bounds = t.getLocalBounds();
        t.setOrigin(bounds.width / 2.f, bounds.height / 2.f);
        t.setPosition(Cfg::WORLD_W / 2.f, Cfg::WORLD_H / 2.f);
        window_.draw(t);
    }

    void updateAndDrawNotifications(float dt) {
        for (auto it = notifications_.begin(); it != notifications_.end();) {
            it->lifetime -= dt;
            if (it->lifetime <= 0.f) {
                it = notifications_.erase(it);
                continue;
            }
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

    //sfml
    sf::RenderWindow window_;
    sf::Font font_;
    sf::CircleShape playerShape_;
    sf::RectangleShape zombieShape_;
    sf::Vertex swordLine_[2];

    //game stats
    InterpEntity playerInterp_;
    std::unordered_map<uint32_t, InterpEntity> zombieInterps_;
    std::unordered_map<uint32_t, int> zombieHp_;
    int playerLives_{3};
    bool playerAlive_{true};
    uint32_t score_{0};
    uint32_t wave_{1};
    std::vector<Notification> notifications_;
};
