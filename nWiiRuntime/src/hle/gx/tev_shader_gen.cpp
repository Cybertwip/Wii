#include "runtime/gx/tev_shader_gen.h"
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace nwii::runtime::gx {

GeneratedShader GenerateTEVShader(const GXState& state, uint8_t prim_type) {
    GeneratedShader shader;

    

    

    
    std::stringstream vs;
    vs << "#version 330 core\n";

    
    
    vs << "layout(location = 0) in vec3 vertexPosition;\n";
    vs << "layout(location = 1) in vec4 vertexColor;\n";
    vs << "layout(location = 2) in vec2 vertexTexCoord;\n";
    vs << "layout(location = 3) in vec3 vertexNormal;\n";
    for (int i = 1; i < 8; ++i)
        vs << "layout(location = " << 3 + i << ") in vec2 vertexTexCoord" << i << ";\n";

    vs << "out vec4 vColor0;\n";
    vs << "out vec4 vColor1;\n";
    for (int i = 0; i < 8; ++i)
        vs << "out vec2 vTex" << i << ";\n";

    vs << "uniform mat4 uTexMtx[8];\n";
    vs << "uniform mat4 mvp;\n";
    // XF colour-channel state (0x100a-0x1011) and the 8 lights (0x600).
    vs << "uniform vec4 uMatColor[2];\n";
    vs << "uniform vec4 uAmbColor[2];\n";
    vs << "uniform int uChanCtrl[4];\n"; // 0,1 = colour chans; 2,3 = alpha chans
    vs << "uniform vec4 uLightCol[8];\n";
    vs << "uniform vec4 uLightPos[8];\n";
    vs << "uniform vec4 uLightDir[8];\n";
    vs << "uniform vec4 uLightCosAtt[8];\n";
    vs << "uniform vec4 uLightDistAtt[8];\n";

    // GX colour channel: the rasterised colour is NOT the vertex colour —
    // it is material (register or vertex) modulated by ambient + lights,
    // per XF channel control. Games animate the material register (fades,
    // the WARNING "press" blink); ignoring it makes those effects vanish.
    vs << R"(
vec4 dolphin_chan(int chan, vec3 pos, vec3 nrm, vec4 vcol) {
    int ctrl = uChanCtrl[chan];
    int actrl = uChanCtrl[chan + 2];
    bool matsrc_vtx  = (ctrl & 1) != 0;
    bool lit         = (ctrl & 2) != 0;
    bool ambsrc_vtx  = (ctrl & 0x40) != 0;
    int  diffusefunc = (ctrl >> 7) & 3;
    int  attnfunc    = (ctrl >> 9) & 3;
    int  lightmask   = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
    bool amatsrc_vtx = (actrl & 1) != 0;
    bool alit        = (actrl & 2) != 0;
    bool aambsrc_vtx = (actrl & 0x40) != 0;

    vec4 mat = vec4(matsrc_vtx ? vcol.rgb : uMatColor[chan].rgb,
                    amatsrc_vtx ? vcol.a : uMatColor[chan].a);
    vec4 lacc = vec4(lit  ? (ambsrc_vtx  ? vcol.rgb : uAmbColor[chan].rgb) : vec3(1.0),
                     alit ? (aambsrc_vtx ? vcol.a   : uAmbColor[chan].a)   : 1.0);

    if (lit) {
        vec3 _normal = normalize(nrm);
        for (int i = 0; i < 8; ++i) {
            if ((lightmask & (1 << i)) == 0) continue;
            vec3 ldir; float attn;
            if (attnfunc == 3) { // spot
                ldir = uLightPos[i].xyz - pos;
                float dist2 = dot(ldir, ldir);
                float dist = sqrt(dist2);
                ldir = ldir / dist;
                attn = max(0.0, dot(ldir, uLightDir[i].xyz));
                attn = max(0.0, uLightCosAtt[i].x + uLightCosAtt[i].y * attn +
                                uLightCosAtt[i].z * attn * attn) /
                       dot(uLightDistAtt[i].xyz, vec3(1.0, dist, dist2));
            } else if (attnfunc == 1) { // specular
                ldir = normalize(uLightPos[i].xyz - pos);
                attn = (dot(_normal, ldir) >= 0.0)
                           ? max(0.0, dot(_normal, uLightDir[i].xyz)) : 0.0;
                vec3 cosAttn = uLightCosAtt[i].xyz;
                vec3 distAttn = (diffusefunc == 0) ? uLightDistAtt[i].xyz
                                                   : normalize(uLightDistAtt[i].xyz);
                attn = max(0.0, dot(cosAttn, vec3(1.0, attn, attn * attn))) /
                       dot(distAttn, vec3(1.0, attn, attn * attn));
            } else { // none / directional
                ldir = uLightPos[i].xyz - pos;
                ldir = (length(ldir) == 0.0) ? _normal : normalize(ldir);
                attn = 1.0;
            }
            float ndl = (diffusefunc == 0) ? 1.0
                      : ((diffusefunc == 1) ? dot(ldir, _normal)
                                            : max(0.0, dot(ldir, _normal)));
            lacc.rgb += attn * ndl * uLightCol[i].rgb;
            lacc.a   += attn * ndl * uLightCol[i].a;
        }
    }
    return clamp(mat * clamp(lacc, 0.0, 1.0), 0.0, 1.0);
}
)";

    vs << "void main() {\n";
    vs << "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n";
    vs << "    vColor0 = dolphin_chan(0, vertexPosition, vertexNormal, vertexColor);\n";
    vs << "    vColor1 = dolphin_chan(1, vertexPosition, vertexNormal, vertexColor);\n";
    for (int i = 0; i < 8; ++i)
        vs << "    vTex" << i << " = (uTexMtx[" << i
           << "] * vec4(vertexTexCoord" << (i ? std::to_string(i) : "") << ", 0.0, 1.0)).xy;\n";
    vs << "}\n";
    shader.vertex_source = vs.str();

    std::stringstream fs;
    fs << "#version 330 core\n";
    fs << "in vec4 vColor0;\n";
    fs << "in vec4 vColor1;\n";
    fs << "in vec2 vTex0;\n";
    fs << "in vec2 vTex1;\n";
    fs << "in vec2 vTex2;\n";
    fs << "in vec2 vTex3;\n";
    fs << "in vec2 vTex4;\n";
    fs << "in vec2 vTex5;\n";
    fs << "in vec2 vTex6;\n";
    fs << "in vec2 vTex7;\n";
    fs << "out vec4 FragColor;\n";
    
    for (int i = 0; i < 8; ++i) {
        fs << "uniform sampler2D uTex" << i << ";\n";
    }
    fs << "uniform vec4 uTevColor[4];\n";
    fs << "uniform vec4 uTevKColor[4];\n";
    
    fs << "vec4 tevReg[4];\n"; 
    fs << "vec4 texColor;\n";
    fs << "vec4 rasColor;\n";
    
    // TEV comparison inputs are unsigned eight-bit values, even when the
    // source register holds a signed intermediate result.
    fs << R"(
ivec3 tev8(vec3 v) { return ivec3(round(v * 255.0)) & ivec3(255); }
int tev8(float v) { return int(round(v * 255.0)) & 255; }
int tevPack(ivec3 v, int mode) {
    if (mode == 0) return v.r;
    if (mode == 1) return v.r | (v.g << 8);
    return v.r | (v.g << 8) | (v.b << 16);
}
)";
    fs << "void main() {\n";
    fs << "    tevReg[0] = uTevColor[0];\n";
    fs << "    tevReg[1] = uTevColor[1];\n";
    fs << "    tevReg[2] = uTevColor[2];\n";
    fs << "    tevReg[3] = uTevColor[3];\n";
    
    uint8_t numTevs = state.numTevStages;
    if (numTevs == 0) numTevs = 1;
    
    for (int i = 0; i < numTevs; ++i) {
        const auto& stage = state.tevStages[i];
        fs << "    { // TEV Stage " << i << "\n";

        if (stage.colorChan == 0) fs << "    rasColor = vColor0;\n";
        else if (stage.colorChan == 1) fs << "    rasColor = vColor1;\n";
        else fs << "    rasColor = vec4(0.0);\n";

        if (stage.texMap != 0xFF) {
            fs << "    texColor = texture(uTex" << (int)stage.texMap << ", vTex" << (int)stage.texCoord << ");\n";
        } else {
            fs << "    texColor = vec4(1.0);\n";
        }
        
        // Each TEV stage selects independent texture and raster swizzle tables.
        auto swap = [&](unsigned table) {
            const char* channels = "rgba";
            uint32_t rg = state.bp[0xF6 + table * 2];
            uint32_t ba = state.bp[0xF7 + table * 2];
            return std::string{channels[rg & 3], channels[(rg >> 2) & 3],
                               channels[ba & 3], channels[(ba >> 2) & 3]};
        };
        const uint32_t alpha_env = state.bp[0xC1 + i * 2];
        fs << "    texColor = texColor." << swap((alpha_env >> 2) & 3) << ";\n";
        fs << "    rasColor = rasColor." << swap(alpha_env & 3) << ";\n";

        uint32_t ksel_reg = state.bp[0xF6 + (i / 2)];
        int kcsel = 0, kasel = 0;
        if ((i % 2) == 0) {
            kcsel = (ksel_reg >> 4) & 0x1F;
            kasel = (ksel_reg >> 9) & 0x1F;
        } else {
            kcsel = (ksel_reg >> 14) & 0x1F;
            kasel = (ksel_reg >> 19) & 0x1F;
        }
        
        auto get_konst_color = [&](int kc) -> std::string {
            if (kc <= 0x07) {
                
                char buf[32];
                std::snprintf(buf, sizeof(buf), "vec3(%.6f)", (8 - kc) / 8.0f);
                return std::string(buf);
            } else if (kc >= 0x0C && kc <= 0x0F) {
                return "uTevKColor[" + std::to_string(kc - 0x0C) + "].rgb";
            } else if (kc >= 0x10 && kc <= 0x1F) {
                int idx = (kc - 0x10) % 4;
                int comp = (kc - 0x10) / 4;
                const char* comps[] = {"r", "g", "b", "a"};
                return "vec3(uTevKColor[" + std::to_string(idx) + "]." + comps[comp] + ")";
            }
            return "vec3(0.0)";
        };

        auto get_konst_alpha = [&](int ka) -> std::string {
            if (ka <= 0x07) {
                
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.6f", (8 - ka) / 8.0f);
                return std::string(buf);
            } else if (ka >= 0x10 && ka <= 0x1F) {
                int idx = (ka - 0x10) % 4;
                int comp = (ka - 0x10) / 4;
                const char* comps[] = {"r", "g", "b", "a"};
                return "uTevKColor[" + std::to_string(idx) + "]." + comps[comp];
            }
            return "0.0";
        };
        std::string kc_str = get_konst_color(kcsel);
        std::string ka_str = get_konst_alpha(kasel);
        
        fs << "    vec3 cIn[4];\n";
        auto map_color_input = [&](int in_idx, int arg) {
            switch(arg) {
                case 0: fs << "    cIn[" << in_idx << "] = tevReg[0].rgb;\n"; break;
                case 1: fs << "    cIn[" << in_idx << "] = vec3(tevReg[0].a);\n"; break;
                case 2: fs << "    cIn[" << in_idx << "] = tevReg[1].rgb;\n"; break;
                case 3: fs << "    cIn[" << in_idx << "] = vec3(tevReg[1].a);\n"; break;
                case 4: fs << "    cIn[" << in_idx << "] = tevReg[2].rgb;\n"; break;
                case 5: fs << "    cIn[" << in_idx << "] = vec3(tevReg[2].a);\n"; break;
                case 6: fs << "    cIn[" << in_idx << "] = tevReg[3].rgb;\n"; break;
                case 7: fs << "    cIn[" << in_idx << "] = vec3(tevReg[3].a);\n"; break;
                case 8: fs << "    cIn[" << in_idx << "] = texColor.rgb;\n"; break;
                case 9: fs << "    cIn[" << in_idx << "] = vec3(texColor.a);\n"; break;
                case 10: fs << "    cIn[" << in_idx << "] = rasColor.rgb;\n"; break;
                case 11: fs << "    cIn[" << in_idx << "] = vec3(rasColor.a);\n"; break;
                case 12: fs << "    cIn[" << in_idx << "] = vec3(1.0);\n"; break;
                case 13: fs << "    cIn[" << in_idx << "] = vec3(0.5);\n"; break;
                case 14: fs << "    cIn[" << in_idx << "] = " << kc_str << ";\n"; break; 
                case 15: fs << "    cIn[" << in_idx << "] = vec3(0.0);\n"; break;
                default: fs << "    cIn[" << in_idx << "] = vec3(0.0);\n"; break;
            }
        };
        map_color_input(0, stage.colorInA);
        map_color_input(1, stage.colorInB);
        map_color_input(2, stage.colorInC);
        map_color_input(3, stage.colorInD);
        
        fs << "    vec3 cOut;\n";
        if (stage.colorBias == 3) {
            const char* op = stage.colorOp ? "==" : ">";
            if (stage.colorScale == 3) {
                const char* fn = stage.colorOp ? "equal" : "greaterThan";
                fs << "    cOut = cIn[3] + mix(vec3(0.0), cIn[2], " << fn
                   << "(tev8(cIn[0]), tev8(cIn[1])));\n";
            } else {
                fs << "    cOut = cIn[3] + ((tevPack(tev8(cIn[0]), "
                   << int(stage.colorScale) << ") " << op
                   << " tevPack(tev8(cIn[1]), " << int(stage.colorScale)
                   << ")) ? cIn[2] : vec3(0.0));\n";
            }
        } else {
            const char* bias = stage.colorBias == 1 ? "0.5" :
                               stage.colorBias == 2 ? "-0.5" : "0.0";
            fs << "    cOut = cIn[3] " << (stage.colorOp ? "-" : "+")
               << " mix(cIn[0], cIn[1], cIn[2]) + vec3(" << bias << ");\n";
            if (stage.colorScale == 1) fs << "    cOut *= 2.0;\n";
            else if (stage.colorScale == 2) fs << "    cOut *= 4.0;\n";
            else if (stage.colorScale == 3) fs << "    cOut *= 0.5;\n";
        }
        if (stage.colorClamp) fs << "    cOut = clamp(cOut, 0.0, 1.0);\n";

        fs << "    float aIn[4];\n";
        auto map_alpha_input = [&](int in_idx, int arg) {
            switch(arg) {
                case 0: fs << "    aIn[" << in_idx << "] = tevReg[0].a;\n"; break;
                case 1: fs << "    aIn[" << in_idx << "] = tevReg[1].a;\n"; break;
                case 2: fs << "    aIn[" << in_idx << "] = tevReg[2].a;\n"; break;
                case 3: fs << "    aIn[" << in_idx << "] = tevReg[3].a;\n"; break;
                case 4: fs << "    aIn[" << in_idx << "] = texColor.a;\n"; break;
                case 5: fs << "    aIn[" << in_idx << "] = rasColor.a;\n"; break;
                case 6: fs << "    aIn[" << in_idx << "] = " << ka_str << ";\n"; break; 
                case 7: fs << "    aIn[" << in_idx << "] = 0.0;\n"; break;
                default: fs << "    aIn[" << in_idx << "] = 0.0;\n"; break;
            }
        };
        map_alpha_input(0, stage.alphaInA);
        map_alpha_input(1, stage.alphaInB);
        map_alpha_input(2, stage.alphaInC);
        map_alpha_input(3, stage.alphaInD);
        
        fs << "    float aOut;\n";
        if (stage.alphaBias == 3) {
            const char* op = stage.alphaOp ? "==" : ">";
            if (stage.alphaScale == 3) {
                fs << "    aOut = aIn[3] + ((tev8(aIn[0]) " << op
                   << " tev8(aIn[1])) ? aIn[2] : 0.0);\n";
            } else {
                fs << "    aOut = aIn[3] + ((tevPack(tev8(cIn[0]), "
                   << int(stage.alphaScale) << ") " << op
                   << " tevPack(tev8(cIn[1]), " << int(stage.alphaScale)
                   << ")) ? aIn[2] : 0.0);\n";
            }
        } else {
            const char* bias = stage.alphaBias == 1 ? "0.5" :
                               stage.alphaBias == 2 ? "-0.5" : "0.0";
            fs << "    aOut = aIn[3] " << (stage.alphaOp ? "-" : "+")
               << " mix(aIn[0], aIn[1], aIn[2]) + " << bias << ";\n";
            if (stage.alphaScale == 1) fs << "    aOut *= 2.0;\n";
            else if (stage.alphaScale == 2) fs << "    aOut *= 4.0;\n";
            else if (stage.alphaScale == 3) fs << "    aOut *= 0.5;\n";
        }
        if (stage.alphaClamp) fs << "    aOut = clamp(aOut, 0.0, 1.0);\n";

        // Both combiners read the register values at the start of the stage.
        fs << "    tevReg[" << int(stage.colorRegId) << "].rgb = cOut;\n";
        fs << "    tevReg[" << (int)stage.alphaRegId << "].a = aOut;\n    }\n";
    }
    
    const auto& last = state.tevStages[numTevs - 1];
    fs << "    FragColor = vec4(tevReg[" << int(last.colorRegId)
       << "].rgb, tevReg[" << int(last.alphaRegId) << "].a);\n";

    
    if (std::getenv("NWII_FLATCOLOR"))
        fs << "    FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n";

    

    {
        const auto& at = state.alphaTest;
        auto cmp = [&](uint8_t comp, uint8_t ref) -> std::string {
            const std::string r = std::to_string(ref);
            switch (comp) {
                case 0: return "false";
                case 1: return "alpha8 < " + r;
                case 2: return "alpha8 == " + r;
                case 3: return "alpha8 <= " + r;
                case 4: return "alpha8 > " + r;
                case 5: return "alpha8 != " + r;
                case 6: return "alpha8 >= " + r;
                default: return "true";
            }
        };
        bool disabled = (at.comp0 == 7 && at.comp1 == 7 && at.logic == 0) ||
                        std::getenv("NWII_NOALPHATEST") != nullptr;
        if (!disabled) {
            fs << "    int alpha8 = int(round(clamp(FragColor.a, 0.0, 1.0) * 255.0));\n";
            std::string a = cmp(at.comp0, at.ref0);
            std::string b = cmp(at.comp1, at.ref1);
            std::string test;
            switch (at.logic) {
                case 0: test = "(" + a + ") && (" + b + ")"; break;
                case 1: test = "(" + a + ") || (" + b + ")"; break;
                case 2: test = "((" + a + ") != (" + b + "))"; break;
                default: test = "((" + a + ") == (" + b + "))"; break;
            }
            fs << "    if (!(" << test << ")) discard;\n";
        }
    }
    
    fs << "}\n";
    shader.fragment_source = fs.str();
    
    return shader;
}

} 
