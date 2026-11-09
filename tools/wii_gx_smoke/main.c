#include <gccore.h>

#define FIFO_SIZE (256 * 1024)
#define XFB_WIDTH 640
#define XFB_HEIGHT 528

static u8 fifo[FIFO_SIZE] ATTRIBUTE_ALIGN(32);
static u8 xfb[XFB_WIDTH * XFB_HEIGHT * 2] ATTRIBUTE_ALIGN(32);

static void draw_color_quad(void)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(-1.0f, -1.0f, 0.0f);
    GX_Color4u8(255, 0, 0, 255);
    GX_Position3f32(1.0f, -1.0f, 0.0f);
    GX_Color4u8(0, 255, 0, 255);
    GX_Position3f32(1.0f, 1.0f, 0.0f);
    GX_Color4u8(0, 0, 255, 255);
    GX_Position3f32(-1.0f, 1.0f, 0.0f);
    GX_Color4u8(255, 255, 0, 255);
    GX_End();
}

int main(void)
{
    GX_Init(MEM_K0_TO_K1(fifo), FIFO_SIZE);

    GX_SetCopyClear((GXColor){16, 24, 40, 255}, GX_MAX_Z24);
    GX_SetViewport(0.0f, 0.0f, (f32)XFB_WIDTH, (f32)XFB_HEIGHT, 0.0f, 1.0f);
    GX_SetScissor(0, 0, XFB_WIDTH, XFB_HEIGHT);
    GX_SetDispCopySrc(0, 0, XFB_WIDTH, XFB_HEIGHT);
    GX_SetDispCopyDst(XFB_WIDTH, XFB_HEIGHT);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetNumTexGens(0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    Mtx position = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f}
    };
    Mtx44 projection = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f}
    };
    GX_LoadPosMtxImm(position, GX_PNMTX0);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);

    for (;;) {
        draw_color_quad();
        GX_CopyDisp(MEM_K0_TO_K1(xfb), GX_TRUE);
        GX_Flush();
    }
}
