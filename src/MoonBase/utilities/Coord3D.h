/**
    @title     MoonBase
    @file      Coord3D.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#ifndef MIN
  #define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
  #define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

struct Coord3D {
  int x;
  int y;
  int z;

  // Coord3D() : x(0), y(0), z(0) {} // Default constructor

  Coord3D() {
    this->x = 0;
    this->y = 0;
    this->z = 0;
  }

  Coord3D(int x, int y = 0, int z = 0) {
    this->x = x;
    this->y = y;
    this->z = z;
  }

  // comparisons
  bool operator!=(const Coord3D& rhs) const {
    return x != rhs.x || y != rhs.y || z != rhs.z;
    // return !(*this==rhs);
  }
  bool operator==(const Coord3D rhs) const { return x == rhs.x && y == rhs.y && z == rhs.z; }
  bool operator<(const int rhs) const { return x < rhs && y < rhs && z < rhs; }

  // Minus / delta (abs)
  Coord3D operator-(const Coord3D& rhs) const { return Coord3D(x - rhs.x, y - rhs.y, z - rhs.z); }
  Coord3D operator-(const int rhs) const { return Coord3D(x - rhs, y - rhs, z - rhs); }
  Coord3D operator+(const Coord3D& rhs) const { return Coord3D(x + rhs.x, y + rhs.y, z + rhs.z); }
  Coord3D operator*(const Coord3D& rhs) const { return Coord3D(x * rhs.x, y * rhs.y, z * rhs.z); }
  Coord3D operator/(const Coord3D& rhs) const {
    Coord3D out(*this);
    out /= rhs;  // keeps current zero-handling contract from operator/=
    return out;
  }
  Coord3D operator/(int rhs) const {
    if (rhs == 0) return *this;
    return Coord3D(x / rhs, y / rhs, z / rhs);
  }
  Coord3D operator%(const Coord3D& rhs) const { return Coord3D(rhs.x ? x % rhs.x : x, rhs.y ? y % rhs.y : y, rhs.z ? z % rhs.z : z); }

  // assignments
  Coord3D operator=(const Coord3D& rhs) {
    x = rhs.x;
    y = rhs.y;
    z = rhs.z;
    return *this;
  }
  Coord3D operator+=(const Coord3D& rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
  }
  Coord3D operator/=(const Coord3D& rhs) {
    if (rhs.x) x /= rhs.x;
    if (rhs.y) y /= rhs.y;
    if (rhs.z) z /= rhs.z;
    return *this;
  }

  Coord3D maximum(const Coord3D& rhs) const { return Coord3D(MAX(x, rhs.x), MAX(y, rhs.y), MAX(z, rhs.z)); }

  unsigned distanceSquared(const Coord3D& rhs) const {
    Coord3D delta = (*this - rhs);
    return (delta.x) * (delta.x) + (delta.y) * (delta.y) + (delta.z) * (delta.z);
  }

  bool isOutofBounds(const Coord3D& rhs) const { return x < 0 || y < 0 || z < 0 || x >= rhs.x || y >= rhs.y || z >= rhs.z; }
};
