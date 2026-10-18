local sokol = require("sokol_clx")

-- Window size (displayed to the user)
local WIN_W, WIN_H = 600, 450

-- Internal render resolution
local W, H = 560, 400
local MAX_ITER = 128
local LOG2 = 0.6931471805599453

local buf = {}
-- Initialize buffer
for i = 1, W * H * 4 do
  buf[i] = 0
end

-- Center of the zoom (a point deep inside the set that stays interesting)
local center_r = -0.743643887037151
local center_i = 0.13182590420533

-- zoom-space: half-width of the view in fractal units
-- (fractal extent spans about [-2.2, 1.2], i.e. ~3.5 units wide)
local half_w = 1.75
local zr_speed = 0.12          -- exponential zoom speed per second

-- NOTE: fractal inputs arrive as parameters, not upvalues, so the compiler
-- keeps them (and hence px/py/zx/zy) as native doubles instead of boxed
-- values. Together with the literal `128` bound below (kept in sync with
-- MAX_ITER) the escape loop compiles to plain double arithmetic with no
-- per-iteration runtime calls. Referring to the MAX_ITER upvalue here, or
-- closing over half_w/center_r/center_i, would box the whole loop (~7x
-- slower when zoomed in, where the average iteration count is highest).
local function compute_row(row, half_w, cr, ci, w, h)
  local vrange = half_w * 2 * (h / w)
  local hw2 = half_w * 2
  local py = ci + (row - h / 2) / h * vrange
  local idx = row * w * 4 + 1
  for x = 0, w - 1 do
    local px = cr + (x - w / 2) / w * hw2
    -- Cached squares: the escape test reuses zr2/zi2 instead of recomputing
    -- zx*zx + zy*zy twice per iteration (once in the test, once in the body).
    local zx, zy, zr2, zi2, iter = 0, 0, 0, 0, 0
    while zr2 + zi2 <= 4 and iter < 128 do
      zy = 2 * zx * zy + py
      zx = zr2 - zi2 + px
      zr2 = zx * zx
      zi2 = zy * zy
      iter = iter + 1
    end
    if iter >= MAX_ITER then
      buf[idx] = 0; buf[idx + 1] = 0; buf[idx + 2] = 0; buf[idx + 3] = 255
    else
      -- Smooth (fractional) iteration count for nicer color bands
      local lzn = math.log(zx * zx + zy * zy) / 2
      local nu = math.log(lzn / LOG2) / LOG2
      local t = (iter + 1 - nu) / MAX_ITER
      local r = math.min(1, t * 4)
      local g = math.min(1, (t - 0.25) * 4)
      local b = math.min(1, (t - 0.5) * 2)
      buf[idx] = math.floor(math.max(0, r) * 255 + 0.5)
      buf[idx + 1] = math.floor(math.max(0, g) * 255 + 0.5)
      buf[idx + 2] = math.floor(math.max(0, b) * 255 + 0.5)
      buf[idx + 3] = 255
    end
    idx = idx + 4
  end
end

local frame_no = 0

sokol.run {
  title = "Mandelbrot Zoom",
  width = WIN_W,
  height = WIN_H,

  frame = function(_dt)
    -- Exponential zoom: each frame magnifies by a constant factor per unit time.
    -- Time-based so the zoom speed is frame-rate independent.
    half_w = half_w * math.exp(-zr_speed * (_dt or 1 / 60))

    -- Interlaced refresh: recompute half the rows per frame, alternating
    -- even/odd rows (the first frame renders everything so no row stays
    -- blank). At this zoom speed the one-frame lag on alternate rows is a
    -- fraction of a pixel, but per-frame cost is halved so the zoom motion
    -- stays smooth instead of stuttering as iteration counts rise.
    if frame_no == 0 then
      for row = 0, H - 1 do
        compute_row(row, half_w, center_r, center_i, W, H)
      end
    else
      for row = frame_no % 2, H - 1, 2 do
        compute_row(row, half_w, center_r, center_i, W, H)
      end
    end
    frame_no = frame_no + 1
    sokol.pixels(W, H, buf)
  end,

  event = function(ev)
    if ev.type == "key_down" and ev.key == "escape" then os.exit(0) end
  end,
}
