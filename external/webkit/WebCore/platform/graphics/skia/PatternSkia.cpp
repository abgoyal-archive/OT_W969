

#include "config.h"
#include "Pattern.h"

#include "AffineTransform.h"
#include "Image.h"
#include "NativeImageSkia.h"

#include "SkCanvas.h"
#include "SkColor.h"
#include "SkColorShader.h"
#include "SkShader.h"

namespace WebCore {

void Pattern::platformDestroy()
{
    m_pattern->safeUnref();
    m_pattern = 0;
}

PlatformPatternPtr Pattern::platformPattern(const AffineTransform& patternTransform)
{
    if (m_pattern)
        return m_pattern;

    // Note: patternTransform is ignored since it seems to be applied elsewhere
    // (when the pattern is used?). Applying it to the pattern (i.e.
    // shader->setLocalMatrix) results in a double transformation. This can be
    // seen, for instance, as an extra offset in:
    // LayoutTests/fast/canvas/patternfill-repeat.html
    // and expanded scale and skew in:
    // LayoutTests/svg/W3C-SVG-1.1/pservers-grad-06-b.svg

    SkBitmap* bm = m_tileImage->nativeImageForCurrentFrame();
    // If we don't have a bitmap, return a transparent shader.
    if (!bm)
        m_pattern = new SkColorShader(SkColorSetARGB(0, 0, 0, 0));

    else if (m_repeatX && m_repeatY)
        m_pattern = SkShader::CreateBitmapShader(*bm, SkShader::kRepeat_TileMode, SkShader::kRepeat_TileMode);

    else {

        // Skia does not have a "draw the tile only once" option. Clamp_TileMode
        // repeats the last line of the image after drawing one tile. To avoid
        // filling the space with arbitrary pixels, this workaround forces the
        // image to have a line of transparent pixels on the "repeated" edge(s),
        // thus causing extra space to be transparent filled.
        SkShader::TileMode tileModeX = m_repeatX ? SkShader::kRepeat_TileMode : SkShader::kClamp_TileMode;
        SkShader::TileMode tileModeY = m_repeatY ? SkShader::kRepeat_TileMode : SkShader::kClamp_TileMode;
        int expandW = m_repeatX ? 0 : 1;
        int expandH = m_repeatY ? 0 : 1;

        // Create a transparent bitmap 1 pixel wider and/or taller than the
        // original, then copy the orignal into it.
        // FIXME: Is there a better way to pad (not scale) an image in skia?
        SkBitmap bm2;
        bm2.setConfig(bm->config(), bm->width() + expandW, bm->height() + expandH);
        bm2.allocPixels();
        bm2.eraseARGB(0x00, 0x00, 0x00, 0x00);
        SkCanvas canvas(bm2);
        canvas.drawBitmap(*bm, 0, 0);
        m_pattern = SkShader::CreateBitmapShader(bm2, tileModeX, tileModeY);
    }
    m_pattern->setLocalMatrix(m_patternSpaceTransformation);
    return m_pattern;
}

void Pattern::setPlatformPatternSpaceTransform()
{
    if (m_pattern)
        m_pattern->setLocalMatrix(m_patternSpaceTransformation);
}

} // namespace WebCore