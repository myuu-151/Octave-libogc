# LightLayer

A layer of light for a 2D game. The lights added each frame are summed per pixel, the sum is cut
into a few hard bands with a 4x4 ordered dither where they meet, and the result is written to a
texture as darkness: black, its alpha how dark. Show that texture on a Quad stretched over the
scene (one layer pixel to one pixel of the picture) and what's lit shows through. Draw anything
with light of its own (flames, eyes) after it, so the darkness never covers it.

Per pixel, each light at distance `d` within its radius `r` gives `(1 - d / r) * 1.7 * strength`,
capped at 1; the sum is capped at 1, cut into bands, and the darkness is
`max(0, darkness - band)`.

On the GameCube and Wii the work is done in C++ each Commit (a 320 x 240 layer is a few
milliseconds), into a 4-bit texture (IA4, a byte a pixel) kept in two buffers taken in turn, so
the GPU never reads one being written. Elsewhere it goes through a dynamic RGBA8 texture.

```lua
-- once
local tex = LightLayer.Init(320, 240)
quad:SetTexture(tex)                       -- a Quad over the scene, the size of the picture

-- each frame
LightLayer.Clear()
LightLayer.AddLight(lanternX, lanternY, 22 + flame * 0.9, 1.0)
LightLayer.AddLight(candleX, candleY, 30, 0.8)
LightLayer.Commit()
```

---
### Init
Sets the layer's size in pixels and makes its texture (again, with a new size, if it changes). On
the GameCube the size is rounded up to a multiple of 8 wide and 4 high.

Sig: `texture = LightLayer.Init(width, height)`
 - Arg: `integer width` Width in pixels
 - Arg: `integer height` Height in pixels
 - Ret: `Texture texture` The layer's texture, or nil if there wasn't the memory
---
### GetTexture
The layer's texture (nil before Init).

Sig: `texture = LightLayer.GetTexture()`
 - Ret: `Texture texture` The layer's texture
---
### SetBands
How many bands the light is cut into (1-15; 5 by default).

Sig: `LightLayer.SetBands(bands)`
 - Arg: `integer bands` Number of bands
---
### SetDarkness
How dark it is where there is no light at all (0-1; 0.97 by default).

Sig: `LightLayer.SetDarkness(darkness)`
 - Arg: `number darkness` Opacity of the darkness
---
### Clear
Removes every light. Call at the start of each frame.

Sig: `LightLayer.Clear()`
---
### AddLight
Adds a light for this frame, in the layer's pixels (radius at most 255).

Sig: `LightLayer.AddLight(x, y, radius, strength=1)`
 - Arg: `number x` X in pixels
 - Arg: `number y` Y in pixels
 - Arg: `number radius` How far it reaches, in pixels
 - Arg: `number strength` How bright (1 lights its middle fully)
---
### Commit
Sums the lights, cuts them into bands and writes the texture. Call once a frame, after the lights.

Sig: `LightLayer.Commit()`
---
