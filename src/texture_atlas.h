#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "vector_util.h"
#include <SFML/Graphics.hpp>

// RectPacker maintains two offsets into an overall bin size:
//  - A line y-offset, representing the current horizontal line on which to place new rectangles
//      horizontally along (the line corresponding to the rectangle tops)
//  - An x-offset into the line, representing the x-coordinate to align the next rectangle left-side
// Furthermore it maintains the maximum texture height encountered along the current line.
// The RectPacker places rectangles left to right along this line until it cannot fit the next
//  rectangle. Once this happens, the line is shifted down to meet the bottom of the tallest texture
//  placed on the line, and the x-offset is reset. This continues until the next rectangle
//  packed cannot be fit on the line within the bin space.
class RectPacker {
public:
    RectPacker(const sf::Vector2f& size) : mSize{size} {}

    // pack returns { succeeded, rect id }. If not succeeded, id is irrelevant (but ideally should
    // be set to the maximum possible value).
    std::pair<bool, size_t> pack(const sf::Vector2f& rectSize) {
        constexpr std::pair<bool, size_t> failure{false, std::numeric_limits<size_t>::max()};
        if (rectSize.x < 0.f || rectSize.y < 0.f || rectSize.x > mSize.x) {
            return failure;
        }

        // Work on tentative copies so that a failed pack leaves the packer state untouched.
        float lineY = mLineY;
        float xOffset = mXOffset;
        float lineHeight = mLineHeight;

        // Doesn't fit on the rest of this line: drop the line to the bottom of the tallest
        // rectangle on it and restart from the left side.
        if (xOffset + rectSize.x > mSize.x) {
            lineY += lineHeight;
            xOffset = 0.f;
            lineHeight = 0.f;
        }

        // Even with the full horizontal space available, we can't fit it vertically.
        if (lineY + rectSize.y > mSize.y) {
            return failure;
        }

        mRects.emplace_back(sf::Vector2f{xOffset, lineY}, rectSize);
        mLineY = lineY;
        mXOffset = xOffset + rectSize.x;
        mLineHeight = std::max(lineHeight, rectSize.y);
        return {true, mRects.size() - 1};
    }

    // Returns the coordinates and size of a previously returned rectangle id.
    // Note that in this coordinate system, y points downwards from the origin at the top-left.
    sf::FloatRect getBounds(size_t rectID) const {
        assert(rectID < mRects.size());
        return mRects[rectID];
    }

private:
    sf::Vector2f mSize;
    float mLineY = 0.f;       // y-coordinate of the current line (tops of rectangles on it)
    float mXOffset = 0.f;     // x-coordinate at which to place the next rectangle on the line
    float mLineHeight = 0.f;  // height of the tallest rectangle on the current line
    std::vector<sf::FloatRect> mRects;
};

// Returns a new image which pages the given img with *pixels* pixels on each side.
// The colour of new border pixels are taken by sampling the closest valid pixel in the original
// image.
//
// NOTE: a production implementation might also support other edge wrapping behaviors, like cycling
// around to the other side of the texture. See https://learnopengl.com/Getting-started/Textures
// Our implementation corresponds to GL_CLAMP_TO_EDGE
inline sf::Image padImage(const sf::Image& img, uint pixels) {
    const sf::Vector2u srcSize = img.getSize();
    if (srcSize.x == 0 || srcSize.y == 0) {
        return img;
    }
    const sf::Vector2u dstSize{srcSize.x + 2 * pixels, srcSize.y + 2 * pixels};
    sf::Image result(dstSize);
    for (unsigned int y = 0; y < dstSize.y; ++y) {
        // closest valid source row: clamp (y - pixels) into [0, srcHeight - 1]
        const unsigned int srcY = std::clamp(static_cast<long long>(y) - pixels, 0LL,
                                             static_cast<long long>(srcSize.y) - 1);
        for (unsigned int x = 0; x < dstSize.x; ++x) {
            const unsigned int srcX = std::clamp(static_cast<long long>(x) - pixels, 0LL,
                                                 static_cast<long long>(srcSize.x) - 1);
            result.setPixel({x, y}, img.getPixel({srcX, srcY}));
        }
    }
    return result;
}

using TextureID = size_t;

class TextureAtlas {
public:
    TextureAtlas(sf::Vector2u binSize, size_t padding)
        : mTexture(binSize), mPacker(sf::Vector2f(mTexture.getSize())), mPadding{padding} {}

    TextureID registerTexture(const std::string& filepath) {
        sf::Image img;
        if (!img.loadFromFile(filepath)) {
            throw std::runtime_error("TextureAtlas: failed to load image " + filepath);
        }
        const sf::Image padded = padImage(img, static_cast<uint>(mPadding));

        const auto [ok, rectID] = mPacker.pack(sf::Vector2f(padded.getSize()));
        if (!ok) {
            throw std::runtime_error("TextureAtlas: no room left in atlas for " + filepath);
        }

        const sf::FloatRect bounds = mPacker.getBounds(rectID);
        mTexture.update(padded, sf::Vector2u(bounds.position));

        mRectIDs.push_back(rectID);
        return mRectIDs.size() - 1;
    }

    // Bounds of the unpadded texture in OpenGL UV space: origin at the bottom-left of the atlas,
    // (1, 1) at the top-right. position is the bottom-left corner of the sub-texture.
    sf::FloatRect getBoundsUV(TextureID id) const {
        const sf::FloatRect px = getBoundsPixels(id);
        const sf::Vector2f atlasSize(mTexture.getSize());
        return {
            {px.position.x / atlasSize.x, 1.f - (px.position.y + px.size.y) / atlasSize.y},
            {px.size.x / atlasSize.x, px.size.y / atlasSize.y},
        };
    }

    sf::Vector2u getSizePixels(TextureID id) const {
        return sf::Vector2u(getBoundsPixels(id).size);
    }

    const sf::Texture& getTexture() const { return mTexture; }

private:
    // Bounds of the unpadded texture in pixels, top-left origin with y pointing down.
    sf::FloatRect getBoundsPixels(TextureID id) const {
        assert(id < mRectIDs.size());
        const sf::FloatRect padded = mPacker.getBounds(mRectIDs[id]);
        const float pad = static_cast<float>(mPadding);
        return {padded.position + sf::Vector2f{pad, pad},
                padded.size - sf::Vector2f{2.f * pad, 2.f * pad}};
    }

    sf::Texture mTexture;
    RectPacker mPacker;
    size_t mPadding;
    std::vector<size_t> mRectIDs;  // TextureID -> packer rect id
};

#endif  // TEXTURE_ATLAS_H
