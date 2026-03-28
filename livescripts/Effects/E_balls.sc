// E_balls.sc - Bouncing Balls
// Simulates bouncing balls with physics, drawn as shaded circles on a 2D grid.
// nrOfBalls: number of active balls (1-25)
// Original: E_balls.sc (StarLight), cross compiled by MoonLight

#define max_nb_balls 25
#define rmax 6
#define rmin 4

uint8_t nrOfBalls = 10;

void setup() {
  addControl(&nrOfBalls, "nrOfBalls", "slider", 1, max_nb_balls);

  for (int i = 0; i < max_nb_balls; i++) {
    Balls[i].vx = random8() / 255.0 + 0.5;
    Balls[i].vy = random8() / 255.0 + 0.3;
    Balls[i].r  = (rmax - rmin) * (random8() / 180.0) + rmin;
    Balls[i].xc = width / 2.0 * (random8() / 255.0 + 0.3) + 15;
    Balls[i].yc = height / 2.0 * (random8() / 255.0 + 0.3) + 15;
    Balls[i].color = random8();
  }
}

struct ball {
  float vx;
  float vy;
  float xc;
  float yc;
  float r;
  int color;

  void drawBall() {
    int startx = xc - r;
    float r2 = r * r;
    float r4 = r * r * r * r;
    int starty = yc - r;
    int _xc = xc;
    int _yc = yc;
    for (int i = startx; i <= _xc; i++) {
      for (int j = starty; j <= _yc; j++) {
        float distance = (i - xc) * (i - xc) + (j - yc) * (j - yc);
        if (distance <= r2) {
          int v = 255 * (1 - distance * distance / r4);
          CRGB cc = hsv(color, 255, v);
          setRGB(i               +  j          * width, cc);
          setRGB((int)(2*xc - i) +  j          * width, cc);
          setRGB((int)(2*xc - i) + (int)(2*yc-j) * width, cc);
          setRGB(i               + (int)(2*yc-j) * width, cc);
        }
      }
    }
  }

  void updateBall() {
    xc = xc + vx;
    yc = yc + vy;
    if (xc >= width  - r - 1) { xc =  width - r - 1.1; vx = -vx; }
    if (xc <  r + 1)          { xc =  r + 1.1;          vx = -vx; }
    if (yc >= height - r - 1) { yc = height - r - 1.1; vy = -vy; }
    if (yc <  r + 1)          { yc =  r + 1.1;          vy = -vy; }
    drawBall();
  }
}

ball Balls[max_nb_balls];
uint32_t h = 1;

void loop() {
  for (int i = 0; i < width; i++)
    for (int j = 0; j < height; j++)
      setRGB(i + width * j, hsv(i + h + j, 255, 180));

  for (int i = 0; i < nrOfBalls; i++)
    Balls[i].updateBall();

  h++;
}
