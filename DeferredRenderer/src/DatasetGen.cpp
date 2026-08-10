#include "DatasetGen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using namespace DirectX;

namespace dr {

// --------------------------------------------------------------------------------------------
// Canonical joint map. Keys are UTF-8 MMD bone names (the project compiles with /utf-8, and
// Saba returns node names as UTF-8, so these literals compare byte-for-byte). A handful of
// English fallbacks cover non-Japanese rigs.
// --------------------------------------------------------------------------------------------
std::string CanonicalJointName(const std::string& n) {
    static const std::unordered_map<std::string, std::string> kMap = {
        // spine chain
        { "全ての親", "root" },    // master / mother bone
        { "センター", "center" },  // body centre — the bone the motion actually translates
        { "上半身3",  "chest2" },  // present on some rigs; gives a third spine segment
        { "下半身",       "pelvis" },  // 下半身
        { "上半身",       "spine" },   // 上半身
        { "上半身2",      "chest" },   // 上半身2
        { "首",                   "neck" },    // 首
        { "頭",                   "head" },    // 頭
        { "左目",             "L_eye" },   // 左目
        { "右目",             "R_eye" },   // 右目
        { "両目",             "eyes_center" },   // MMD's gaze control bone between the eyes
        // Fingers. MMD's standard chain per hand is 親指０/１/２ (thumb: metacarpal + 2 phalanges),
        // 人指 (index), 中指 (middle), 薬指 (ring), 小指 (little), each １/２/３, plus a 先 tip bone.
        // Numerals are FULL-WIDTH (U+FF10..) — these literals must match the rig byte-for-byte.
        { "左親指０", "L_thumb1" },  { "左親指１", "L_thumb2" },  { "左親指２", "L_thumb3" },  { "左親指先", "L_thumb_tip" },
        { "右親指０", "R_thumb1" },  { "右親指１", "R_thumb2" },  { "右親指２", "R_thumb3" },  { "右親指先", "R_thumb_tip" },
        { "左人指１", "L_index1" },  { "左人指２", "L_index2" },  { "左人指３", "L_index3" },  { "左人指先", "L_index_tip" },
        { "右人指１", "R_index1" },  { "右人指２", "R_index2" },  { "右人指３", "R_index3" },  { "右人指先", "R_index_tip" },
        { "左中指１", "L_middle1" }, { "左中指２", "L_middle2" }, { "左中指３", "L_middle3" }, { "左中指先", "L_middle_tip" },
        { "右中指１", "R_middle1" }, { "右中指２", "R_middle2" }, { "右中指３", "R_middle3" }, { "右中指先", "R_middle_tip" },
        { "左薬指１", "L_ring1" },   { "左薬指２", "L_ring2" },   { "左薬指３", "L_ring3" },   { "左薬指先", "L_ring_tip" },
        { "右薬指１", "R_ring1" },   { "右薬指２", "R_ring2" },   { "右薬指３", "R_ring3" },   { "右薬指先", "R_ring_tip" },
        { "左小指１", "L_pinky1" },  { "左小指２", "L_pinky2" },  { "左小指３", "L_pinky3" },  { "左小指先", "L_pinky_tip" },
        { "右小指１", "R_pinky1" },  { "右小指２", "R_pinky2" },  { "右小指３", "R_pinky3" },  { "右小指先", "R_pinky_tip" },
        // arms (腕 = shoulder joint, ひじ = elbow, 手首 = wrist, 肩 = clavicle)
        { "左肩",   "L_clavicle" },        // 左肩
        { "右肩",   "R_clavicle" },        // 右肩
        { "左腕",   "L_shoulder" },        // 左腕
        { "右腕",   "R_shoulder" },        // 右腕
        { "左ひじ", "L_elbow" },       // 左ひじ
        { "右ひじ", "R_elbow" },       // 右ひじ
        { "左手首", "L_wrist" },       // 左手首
        { "右手首", "R_wrist" },       // 右手首
        // legs
        { "左足",   "L_hip" },             // 左足
        { "右足",   "R_hip" },             // 右足
        { "左ひざ", "L_knee" },        // 左ひざ
        { "右ひざ", "R_knee" },        // 右ひざ
        { "左足首", "L_ankle" },       // 左足首
        { "右足首", "R_ankle" },       // 右足首
        { "左つま先", "L_toe" },   // 左つま先
        { "右つま先", "R_toe" },   // 右つま先
        // English fallbacks for non-Japanese rigs
        { "head", "head" }, { "neck", "neck" }, { "spine", "spine" }, { "chest", "chest" },
        { "hips", "pelvis" }, { "pelvis", "pelvis" }, { "root", "root" },
    };
    auto it = kMap.find(n);
    return it != kMap.end() ? it->second : std::string{};
}

const std::vector<std::string>& CanonicalJointOrder() {
    static const std::vector<std::string> kOrder = [] {
        std::vector<std::string> o = {
            // body chain
            "root", "center", "pelvis", "spine", "chest", "chest2", "neck", "head",
            "L_eye", "R_eye", "eyes_center",
            "L_clavicle", "L_shoulder", "L_elbow", "L_wrist",
            "R_clavicle", "R_shoulder", "R_elbow", "R_wrist",
            "L_hip", "L_knee", "L_ankle", "L_toe",
            "R_hip", "R_knee", "R_ankle", "R_toe",
        };
        for (const char* side : { "L", "R" })
            for (const char* f : { "thumb", "index", "middle", "ring", "pinky" })
                for (const char* seg : { "1", "2", "3", "_tip" })
                    o.push_back(std::string(side) + "_" + f + seg);
        return o;
    }();
    return kOrder;
}

bool IsCoreCanonicalJoint(const std::string& c) {
    static const std::unordered_set<std::string> kCore = {
        "pelvis","spine","chest","neck","head",
        "L_clavicle","L_shoulder","L_elbow","L_wrist",
        "R_clavicle","R_shoulder","R_elbow","R_wrist",
        "L_hip","L_knee","L_ankle","L_toe",
        "R_hip","R_knee","R_ankle","R_toe" };
    return kCore.count(c) > 0;
}

const char* BodyPartName(BodyPart p) {
    switch (p) {
        case BodyPart::FullBody:  return "FullBody";
        case BodyPart::UpperBody: return "UpperBody";
        case BodyPart::Face:      return "Face";
        case BodyPart::Arms:      return "Arms";
        case BodyPart::Legs:      return "Legs";
        case BodyPart::Torso:     return "Torso";
        default:                  return "FullBody";
    }
}

bool CanonicalInPart(const std::string& c, BodyPart p) {
    if (c.empty()) return false;
    if (p == BodyPart::FullBody) return true;
    // Fingers belong to the hand they hang off, so an "arms" or "upper body" crop must frame them
    // too — otherwise a close-up of the arms cuts the hands off at the wrist.
    const bool finger = c.size() > 2 && (c[0] == 'L' || c[0] == 'R') && c[1] == '_' &&
                        (c.find("thumb")  != std::string::npos || c.find("index") != std::string::npos ||
                         c.find("middle") != std::string::npos || c.find("ring")  != std::string::npos ||
                         c.find("pinky")  != std::string::npos);
    if (finger) return p == BodyPart::UpperBody || p == BodyPart::Arms;
    static const std::unordered_set<std::string> kUpper = {
        "root","center","pelvis","spine","chest","chest2","neck","head","L_eye","R_eye","eyes_center",
        "L_clavicle","R_clavicle","L_shoulder","R_shoulder","L_elbow","R_elbow","L_wrist","R_wrist" };
    static const std::unordered_set<std::string> kFace = { "head","neck","L_eye","R_eye","eyes_center" };
    static const std::unordered_set<std::string> kArms = {
        "L_clavicle","R_clavicle","L_shoulder","R_shoulder","L_elbow","R_elbow","L_wrist","R_wrist" };
    static const std::unordered_set<std::string> kLegs = {
        "pelvis","L_hip","R_hip","L_knee","R_knee","L_ankle","R_ankle","L_toe","R_toe" };
    static const std::unordered_set<std::string> kTorso = {
        "root","center","pelvis","spine","chest","chest2","neck" };
    switch (p) {
        case BodyPart::UpperBody: return kUpper.count(c) > 0;
        case BodyPart::Face:      return kFace.count(c) > 0;
        case BodyPart::Arms:      return kArms.count(c) > 0;
        case BodyPart::Legs:      return kLegs.count(c) > 0;
        case BodyPart::Torso:     return kTorso.count(c) > 0;
        default:                  return true;
    }
}

const char* StyleName(StyleId s) {
    switch (s) {
        case StyleId::CelFull:     return "cel_full";
        case StyleId::FlatAlbedo:  return "flat_albedo";
        case StyleId::ShadedNoPost:return "shaded_nopost";
        case StyleId::NormalMap:   return "normal";
        case StyleId::Depth:       return "depth";
        case StyleId::Outline:     return "outline";
        case StyleId::HighSat:     return "high_sat";
        case StyleId::LowKey:      return "low_key";
        case StyleId::RimLight:    return "rim_light";
        case StyleId::RandomLight: return "random_light";
        case StyleId::CharDepth:   return "char_depth";
        default:                   return "cel_full";
    }
}

long long EstimateSampleCount(const DatasetConfig& c, double /*motionDurationSec*/) {
    const long long base  = std::max(1, c.azimuthCount) * (long long)std::max(1, c.elevCount);
    const long long crops = c.enableCrops ? (long long)std::lround(c.cropProb * (double)base) : 0;
    const long long views = base + crops;
    const long long styles = c.styles.empty() ? (long long)StyleId::Count : (long long)c.styles.size();
    const long long bg = (c.bgIsolated ? 1 : 0) + (c.bgScene ? 1 : 0);
    return (long long)std::max(1, c.frameCount) * views * styles * std::max(1LL, bg);
}

// --------------------------------------------------------------------------------------------
// JSON serialization (small hand-rolled writer; no external dependency).
// --------------------------------------------------------------------------------------------
namespace {

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else          { o += ch; }   // UTF-8 bytes pass through unchanged
        }
    }
    return o;
}

void Vec3(std::string& o, const XMFLOAT3& v) {
    char b[96]; std::snprintf(b, sizeof b, "[%.5f,%.5f,%.5f]", v.x, v.y, v.z); o += b;
}
void Vec4(std::string& o, const XMFLOAT4& v) {
    char b[128]; std::snprintf(b, sizeof b, "[%.6f,%.6f,%.6f,%.6f]", v.x, v.y, v.z, v.w); o += b;
}
void Mat4(std::string& o, const XMFLOAT4X4& m) {
    char b[320];
    std::snprintf(b, sizeof b,
        "[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f]",
        m._11,m._12,m._13,m._14, m._21,m._22,m._23,m._24,
        m._31,m._32,m._33,m._34, m._41,m._42,m._43,m._44);
    o += b;
}

void WriteMetaFields(std::string& o, const SampleMeta& m) {
    char b[256];
    o += "  \"character\": \""; o += JsonEscape(m.character); o += "\",\n";
    o += "  \"motion\": \"";    o += JsonEscape(m.motion);    o += "\",\n";
    std::snprintf(b, sizeof b, "  \"frame_index\": %d,\n  \"time_sec\": %.4f,\n", m.frameIndex, m.timeSec); o += b;
    std::snprintf(b, sizeof b, "  \"view_index\": %d,\n  \"azimuth_deg\": %.2f,\n  \"elevation_deg\": %.2f,\n",
                  m.viewIndex, m.azimuthDeg, m.elevDeg); o += b;
    o += "  \"background\": \""; o += JsonEscape(m.background); o += "\",\n";
    o += "  \"body_part\": \"";  o += JsonEscape(m.bodyPart);   o += "\",\n";
    // Every image this annotation covers. Shading style does not move the skeleton, so one pose
    // record serves all of them.
    o += "  \"images\": [";
    for (size_t i = 0; i < m.styleImages.size(); ++i) {
        o += "\n    {\"style\": \""; o += JsonEscape(m.styleImages[i].first);
        o += "\", \"file\": \"";     o += JsonEscape(m.styleImages[i].second); o += "\"}";
        if (i + 1 < m.styleImages.size()) o += ",";
    }
    o += m.styleImages.empty() ? "],\n" : "\n  ],\n";
    std::snprintf(b, sizeof b, "  \"image_w\": %d,\n  \"image_h\": %d,\n", m.imgW, m.imgH); o += b;
    // Occlusion summary — filter a training set on visible_fraction without opening the joint list.
    std::snprintf(b, sizeof b,
                  "  \"canonical_in_frame\": %d,\n  \"canonical_visible\": %d,\n  \"visible_fraction\": %.4f,\n",
                  m.canonicalInFrame, m.canonicalVisible, m.visibleFraction); o += b;
    // Scale of the char_depth style image: white = charDepthNear, black = charDepthFar.
    std::snprintf(b, sizeof b, "  \"char_depth_near\": %.3f,\n  \"char_depth_far\": %.3f,\n",
                  m.charDepthNear, m.charDepthFar); o += b;

    o += "  \"camera\": {\n";
    o += "    \"eye\": ";    Vec3(o, m.camEye);    o += ",\n";
    o += "    \"target\": "; Vec3(o, m.camTarget); o += ",\n";
    o += "    \"up\": ";     Vec3(o, m.camUp);     o += ",\n";
    std::snprintf(b, sizeof b, "    \"fov_y\": %.6f,\n    \"aspect\": %.6f,\n    \"z_near\": %.4f,\n    \"z_far\": %.4f,\n",
                  m.fovY, m.aspect, m.zNear, m.zFar); o += b;
    o += "    \"view\": ";  Mat4(o, m.view); o += ",\n";
    o += "    \"proj\": ";  Mat4(o, m.proj); o += "\n";
    o += "  }";
}

} // namespace

std::vector<JointRecord> CompactToCanonical(const std::vector<JointRecord>& all) {
    // Which source bone supplies each canonical joint (first wins if a rig tags two).
    std::unordered_map<std::string, int> srcOf;
    for (int i = 0; i < static_cast<int>(all.size()); ++i)
        if (!all[i].canonical.empty()) srcOf.emplace(all[i].canonical, i);

    std::vector<int> keep;                       // source indices, in canonical order
    std::unordered_map<int, int> outOf;          // source index -> position in the output
    keep.reserve(CanonicalJointOrder().size());
    for (const auto& name : CanonicalJointOrder()) {
        auto it = srcOf.find(name);
        if (it == srcOf.end()) continue;         // rig does not have this joint
        outOf.emplace(it->second, static_cast<int>(keep.size()));
        keep.push_back(it->second);
    }

    std::vector<JointRecord> out;
    out.reserve(keep.size());
    for (int src : keep) {
        JointRecord r = all[src];
        // Walk up the ORIGINAL chain to the nearest ancestor that survived the filter, so the
        // compact skeleton keeps the real hierarchy (e.g. wrist -> elbow even though twist bones
        // sat between them in the rig).
        int p = all[src].parent, parentOut = -1;
        while (p >= 0 && p < static_cast<int>(all.size())) {
            auto it = outOf.find(p);
            if (it != outOf.end()) { parentOut = it->second; break; }
            p = all[p].parent;
        }
        r.parent = parentOut;
        out.push_back(std::move(r));
    }
    return out;
}

bool WriteSampleJson(const std::wstring& jsonPath, const SampleMeta& meta,
                     const std::vector<JointRecord>& joints) {
    std::ofstream f(jsonPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return false;

    std::string o;
    o.reserve(joints.size() * 220 + 1024);
    o += "{\n";
    WriteMetaFields(o, meta);
    o += ",\n  \"joints\": [\n";
    for (size_t i = 0; i < joints.size(); ++i) {
        const JointRecord& j = joints[i];
        char b[128];
        o += "    {\"name\": \""; o += JsonEscape(j.name); o += "\", \"canonical\": \"";
        o += JsonEscape(j.canonical); o += "\", ";
        std::snprintf(b, sizeof b, "\"parent\": %d, ", j.parent); o += b;
        o += "\"world_pos\": ";  Vec3(o, j.worldPos); o += ", ";
        o += "\"world_rot\": ";  Vec4(o, j.worldRot); o += ", ";
        o += "\"local_pos\": ";  Vec3(o, j.localPos); o += ", ";
        o += "\"local_rot\": ";  Vec4(o, j.localRot); o += ", ";
        // in_frame = projects inside the image; occluded = in frame but something is drawn in front
        // of it; visible = in frame and not occluded. Kept as three separate flags because
        // "off-screen" and "hidden behind the torso" are different cases for a training loss.
        std::snprintf(b, sizeof b,
                      "\"px\": %.2f, \"py\": %.2f, \"in_frame\": %s, \"occluded\": %s, "
                      "\"visible\": %s, \"depth\": %.2f, \"surface_depth\": %.2f}",
                      j.px, j.py,
                      j.inFrame ? "true" : "false",
                      (j.inFrame && !j.visible) ? "true" : "false",
                      j.visible ? "true" : "false",
                      j.viewDepth, j.surfaceDepth);
        o += b;
        o += (i + 1 < joints.size()) ? ",\n" : "\n";
    }
    o += "  ]\n}\n";

    f.write(o.data(), (std::streamsize)o.size());
    return f.good();
}

bool AppendManifestLine(const std::wstring& manifestPath, const SampleMeta& m) {
    std::ofstream f(manifestPath.c_str(), std::ios::binary | std::ios::app);
    if (!f) return false;
    char b[256];
    std::string o = "{";
    o += "\"image\":\"";     o += JsonEscape(m.imageFile); o += "\",";
    o += "\"ann\":\"";       o += JsonEscape(m.annFile);   o += "\",";   // shared pose annotation
    o += "\"character\":\""; o += JsonEscape(m.character); o += "\",";
    o += "\"motion\":\"";    o += JsonEscape(m.motion);    o += "\",";
    std::snprintf(b, sizeof b, "\"frame\":%d,\"time\":%.4f,\"view\":%d,", m.frameIndex, m.timeSec, m.viewIndex); o += b;
    std::snprintf(b, sizeof b, "\"az\":%.1f,\"el\":%.1f,", m.azimuthDeg, m.elevDeg); o += b;
    o += "\"style\":\"";     o += JsonEscape(m.style);      o += "\",";
    o += "\"bg\":\"";        o += JsonEscape(m.background);  o += "\",";
    o += "\"part\":\"";      o += JsonEscape(m.bodyPart);    o += "\",";
    std::snprintf(b, sizeof b, "\"vis\":%.4f,\"vis_joints\":%d,\"in_frame_joints\":%d}\n",
                  m.visibleFraction, m.canonicalVisible, m.canonicalInFrame); o += b;
    f.write(o.data(), (std::streamsize)o.size());
    return f.good();
}

} // namespace dr
