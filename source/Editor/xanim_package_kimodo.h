#ifndef XANIM_PACKAGE_KIMODO_H
#define XANIM_PACKAGE_KIMODO_H
#pragma once

// Text to motion in the Animation Package editor, with NVIDIA Kimodo run by xkimodo.exe (dependencies/xKimodo: C++, ggml/llama.cpp; it keeps
// its models loaded in a server of its own and exits when idle). The flow: a prompt, a length and a seed -> Generate (xkimodo writes a BVH
// in the project's Cache\Temp, the editor never waits on it) -> the motion previews on Kimodo's SOMA 77-bone skeleton (the Source of the
// package's Kimodo Retarget) -> Accept saves it in the package's Kimodo folder (a .bvh and a .kimodo.txt with the prompt) and adds it as a
// "Different skeleton: animation asset" source with that Retarget; Discard drops it. Nothing reaches the assets before Accept.
// Where xkimodo is: XKIMODO_EXE, else dependencies\xKimodo\build\{cuda,vulkan,cpu}\Release\xkimodo.exe up from the editor's folder;
// its models: XKIMODO_MODELS, else xkimodo's own default (its folder's models\).

#include <windows.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace xanim_package_editor::kimodo
{
    //--------------------------------------------------------------------------------------------
    // A BVH as xkimodo writes it (any rotation channel order is read): names, parents, offsets, per frame the root position and the
    // local rotations as 3x3 (row major), meters (the file is in centimeters).
    //--------------------------------------------------------------------------------------------
    struct bvh
    {
        std::vector<std::string>    m_Names;
        std::vector<int>            m_Parents;
        std::vector<float>          m_Offsets;      // [joint][3] meters
        std::vector<float>          m_Root;         // [frame][3] meters
        std::vector<float>          m_Rots;         // [frame][joint][9]
        int                         m_nFrames = 0;
        float                       m_FrameTime = 1.0f / 30.0f;
        bool                        m_bRootChannels = false;    // the root has position channels (absolute, OFFSET is only its rest)

        int nJoints() const noexcept { return int(m_Names.size()); }

        static void Axis(char A, float Deg, float* M) noexcept
        {
            const float r = Deg * 0.017453292519943295f, c = std::cos(r), s = std::sin(r);
            const float X[9] = { 1, 0, 0, 0, c, -s, 0, s, c }, Y[9] = { c, 0, s, 0, 1, 0, -s, 0, c }, Z[9] = { c, -s, 0, s, c, 0, 0, 0, 1 };
            std::memcpy(M, A == 'X' ? X : A == 'Y' ? Y : Z, sizeof(X));
        }
        static void Mul(const float* A, const float* B, float* C) noexcept
        {
            float T[9];
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) T[r * 3 + c] = A[r * 3] * B[c] + A[r * 3 + 1] * B[3 + c] + A[r * 3 + 2] * B[6 + c];
            std::memcpy(C, T, sizeof(T));
        }

        bool Load(const std::wstring& Path, std::string& Error) noexcept
        {
            std::ifstream F{ std::filesystem::path(Path) };
            if (!F) { Error = "cannot open the BVH"; return false; }
            std::vector<std::string> Channels;             // per joint: the rotation axes in order, e.g. "ZYX"
            std::vector<int> PosChannel;                   // per joint: 1 when it has position channels
            std::vector<int> Stack;
            std::string Tok;
            int Pending = -1;
            while (F >> Tok)
            {
                if (Tok == "ROOT" || Tok == "JOINT")
                {
                    std::string Name; F >> Name;
                    m_Parents.push_back(Stack.empty() ? -1 : Stack.back());
                    m_Names.push_back(Name); m_Offsets.insert(m_Offsets.end(), { 0, 0, 0 }); Channels.emplace_back(); PosChannel.push_back(0);
                    Pending = nJoints() - 1;
                }
                else if (Tok == "End") { std::string Site; F >> Site; Pending = -2; }
                else if (Tok == "{") Stack.push_back(Pending);
                else if (Tok == "}") { if (!Stack.empty()) Stack.pop_back(); }
                else if (Tok == "OFFSET")
                {
                    float x, y, z; F >> x >> y >> z;
                    const int j = Stack.empty() ? -1 : Stack.back();
                    if (j >= 0) { m_Offsets[j * 3] = x * 0.01f; m_Offsets[j * 3 + 1] = y * 0.01f; m_Offsets[j * 3 + 2] = z * 0.01f; }
                }
                else if (Tok == "CHANNELS")
                {
                    int n; F >> n;
                    const int j = Stack.back();
                    for (int i = 0; i < n; ++i) { std::string C; F >> C; if (C.find("rotation") != std::string::npos) Channels[j] += C[0]; else PosChannel[j] = 1; }
                }
                else if (Tok == "MOTION") break;
            }
            // End Site blocks push -2 as their "joint": Stack holds -2 for them, harmless (OFFSET skips negatives)
            std::string W; F >> W >> m_nFrames;                                   // Frames: N
            F >> W >> W >> m_FrameTime;                                          // Frame Time: t
            const int J = nJoints();
            m_bRootChannels = J > 0 && PosChannel[0];
            if (J == 0 || m_nFrames <= 0) { Error = "the BVH has no joints or no frames"; return false; }
            m_Root.assign(size_t(m_nFrames) * 3, 0.0f);
            m_Rots.assign(size_t(m_nFrames) * J * 9, 0.0f);
            for (int t = 0; t < m_nFrames; ++t)
                for (int j = 0; j < J; ++j)
                {
                    if (PosChannel[j]) for (int k = 0; k < 3; ++k) { float v; F >> v; if (m_Parents[j] < 0) m_Root[t * 3 + k] = v * 0.01f; }
                    float* R = &m_Rots[(size_t(t) * J + j) * 9];
                    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; std::memcpy(R, I, sizeof(I));
                    for (char A : Channels[j]) { float d; F >> d; float M[9]; Axis(A, d, M); Mul(R, M, R); }
                }
            if (!F) { Error = "the BVH ends early"; return false; }
            return true;
        }

        // World position and the bone's right/up axes of every joint at frame t (FK: offsets, rotations, the root position)
        void Pose(int t, std::vector<float>& Pos, std::vector<float>& World) const noexcept
        {
            const int J = nJoints();
            Pos.resize(size_t(J) * 3); World.resize(size_t(J) * 9);
            for (int j = 0; j < J; ++j)
            {
                const float* L = &m_Rots[(size_t(t) * J + j) * 9];
                const int p = m_Parents[j];
                if (p < 0)
                {
                    std::memcpy(&World[j * 9], L, 9 * sizeof(float));
                    for (int k = 0; k < 3; ++k) Pos[j * 3 + k] = m_bRootChannels ? m_Root[t * 3 + k] : m_Offsets[j * 3 + k];   // position channels are absolute
                    continue;
                }
                Mul(&World[p * 9], L, &World[j * 9]);
                const float* P = &World[p * 9]; const float* o = &m_Offsets[j * 3];
                for (int k = 0; k < 3; ++k) Pos[j * 3 + k] = Pos[p * 3 + k] + P[k * 3] * o[0] + P[k * 3 + 1] * o[1] + P[k * 3 + 2] * o[2];
            }
        }
    };

    // row-major 3x3 rotation -> quaternion (x, y, z, w)
    inline void MatToQuat(const float* m, float* q) noexcept
    {
        const float t = m[0] + m[4] + m[8];
        if (t > 0)                          { const float s = std::sqrt(t + 1.0f) * 2;               q[3] = 0.25f * s; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s; }
        else if (m[0] > m[4] && m[0] > m[8]) { const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2; q[3] = (m[7] - m[5]) / s; q[0] = 0.25f * s; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s; }
        else if (m[4] > m[8])               { const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2; q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = 0.25f * s; q[2] = (m[5] + m[7]) / s; }
        else                                { const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2; q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = 0.25f * s; }
    }

    //--------------------------------------------------------------------------------------------
    // One xkimodo run, never waited on: started, then polled once a frame
    //--------------------------------------------------------------------------------------------
    inline std::wstring FindExe() noexcept
    {
        if (const wchar_t* E = _wgetenv(L"XKIMODO_EXE"); E && *E) return E;
        wchar_t Buf[MAX_PATH]; GetModuleFileNameW(nullptr, Buf, MAX_PATH);
        for (auto Dir = std::filesystem::path(Buf).parent_path(); !Dir.empty() && Dir != Dir.root_path(); Dir = Dir.parent_path())
            for (const wchar_t* B : { L"cuda", L"vulkan", L"cpu" })
                if (auto P = Dir / L"dependencies" / L"xKimodo" / L"build" / B / L"Release" / L"xkimodo.exe"; std::filesystem::exists(P)) return P.wstring();
        return {};
    }

    struct job
    {
        PROCESS_INFORMATION     m_PI{};
        bool                    m_bRunning = false;
        double                  m_StartTime = 0;
        std::wstring            m_Bvh, m_Log;

        bool Start(const std::wstring& Exe, const std::wstring& TempDir, const std::string& Prompt, float Seconds, int Seed, std::string& Error) noexcept
        {
            static int s_Count = 0;
            std::filesystem::create_directories(TempDir);
            const auto Stem = std::format(L"{}\\kimodo_{}_{}", TempDir, GetCurrentProcessId(), ++s_Count);
            m_Bvh = Stem + L".bvh"; m_Log = Stem + L".log";
            std::error_code Ec; std::filesystem::remove(m_Bvh, Ec);

            std::wstring P = xstrtool::To(Prompt);
            for (auto& c : P) if (c == L'"' || c == L'\n' || c == L'\r') c = L' ';
            std::wstring Cmd = std::format(L"\"{}\" gen --prompt \"{}\" --seconds {:.2f} --seed {} --bvh \"{}\"", Exe, P, Seconds, Seed, m_Bvh);
            if (const wchar_t* M = _wgetenv(L"XKIMODO_MODELS"); M && *M) Cmd += std::format(L" --models \"{}\"", M);

            SECURITY_ATTRIBUTES SA{ sizeof(SA), nullptr, TRUE };
            HANDLE hLog = CreateFileW(m_Log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &SA, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            STARTUPINFOW SI{ sizeof(SI) };
            SI.dwFlags = STARTF_USESTDHANDLES; SI.hStdOutput = hLog; SI.hStdError = hLog; SI.hStdInput = nullptr;
            const BOOL Ok = CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &SI, &m_PI);
            if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
            if (!Ok) { Error = std::format("xkimodo could not start (error {})", GetLastError()); return false; }
            m_bRunning = true; m_StartTime = ImGui::GetTime();
            return true;
        }

        // true when it just ended; Ok tells whether the BVH is there
        bool Poll(bool& Ok, std::string& Message) noexcept
        {
            if (!m_bRunning || WaitForSingleObject(m_PI.hProcess, 0) != WAIT_OBJECT_0) return false;
            DWORD Code = 1; GetExitCodeProcess(m_PI.hProcess, &Code);
            CloseHandle(m_PI.hProcess); CloseHandle(m_PI.hThread); m_PI = {}; m_bRunning = false;
            std::ifstream L{ std::filesystem::path(m_Log) }; std::stringstream S; S << L.rdbuf(); Message = S.str();
            while (!Message.empty() && (Message.back() == '\n' || Message.back() == '\r')) Message.pop_back();
            Ok = Code == 0 && std::filesystem::exists(m_Bvh);
            return true;
        }

        void Cancel() noexcept
        {
            if (!m_bRunning) return;
            TerminateProcess(m_PI.hProcess, 1);      // the client only: the server keeps running and finishes its one request
            CloseHandle(m_PI.hProcess); CloseHandle(m_PI.hThread); m_PI = {}; m_bRunning = false;
        }

        ~job() { Cancel(); }
    };

    //--------------------------------------------------------------------------------------------
    // Constraints (Suggest Pose): Kimodo's own JSON (kimodo/constraints.py get_save_info), so one file drives Python and xkimodo alike
    //--------------------------------------------------------------------------------------------
    // Kimodo's SOMA 77 joints in its own index order (Kimodo-SOMA-RP-v1.2; the BVH xkimodo writes lists them in this order too)
    // ponytail: the model's skeleton as a constant; read it from the model if another skeleton family is ever generated
    inline constexpr const char* soma77_names_v[77] =
    { "Hips", "Spine1", "Spine2", "Chest", "Neck1", "Neck2", "Head", "HeadEnd", "Jaw", "LeftEye", "RightEye", "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand"
    , "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", "LeftHandThumbEnd", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", "LeftHandIndex4", "LeftHandIndexEnd"
    , "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftHandMiddle4", "LeftHandMiddleEnd", "LeftHandRing1", "LeftHandRing2", "LeftHandRing3", "LeftHandRing4"
    , "LeftHandRingEnd", "LeftHandPinky1", "LeftHandPinky2", "LeftHandPinky3", "LeftHandPinky4", "LeftHandPinkyEnd", "RightShoulder", "RightArm", "RightForeArm", "RightHand"
    , "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandThumbEnd", "RightHandIndex1", "RightHandIndex2", "RightHandIndex3", "RightHandIndex4", "RightHandIndexEnd"
    , "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", "RightHandMiddle4", "RightHandMiddleEnd", "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandRing4"
    , "RightHandRingEnd", "RightHandPinky1", "RightHandPinky2", "RightHandPinky3", "RightHandPinky4", "RightHandPinkyEnd", "LeftLeg", "LeftShin", "LeftFoot", "LeftToeBase"
    , "LeftToeEnd", "RightLeg", "RightShin", "RightFoot", "RightToeBase", "RightToeEnd" };

    struct soma_pose { float m_Rot[77][3] = {}; float m_Root[3] = {}; };      // local axis-angle per joint, root (Hips) position in metres
    struct constraint_set
    {
        std::string                         m_Type;         // "left-hand", "right-hand", "left-foot", "right-foot", "fullbody", "root2d"
        std::vector<int>                    m_Frames;
        std::vector<soma_pose>              m_Poses;        // one per frame (not for root2d)
        std::vector<std::array<float, 2>>   m_Smooth;       // the root's (x, z) per frame
    };

    inline bool WriteConstraints(const std::wstring& Path, const std::vector<constraint_set>& Sets, std::string& Error)
    {
        std::ofstream F{ std::filesystem::path(Path) };
        if (!F) { Error = "cannot write the constraints file"; return false; }
        F.precision(9);
        F << "[\n";
        for (std::size_t s = 0; s < Sets.size(); ++s)
        {
            const auto& C = Sets[s];
            F << "  {\"type\": \"" << C.m_Type << "\", \"frame_indices\": [";
            for (std::size_t i = 0; i < C.m_Frames.size(); ++i) F << (i ? ", " : "") << C.m_Frames[i];
            F << "]";
            if (!C.m_Poses.empty())
            {
                F << ", \"local_joints_rot\": [";
                for (std::size_t i = 0; i < C.m_Poses.size(); ++i)
                {
                    F << (i ? ", " : "") << "[";
                    for (int j = 0; j < 77; ++j) F << (j ? ", " : "") << "[" << C.m_Poses[i].m_Rot[j][0] << ", " << C.m_Poses[i].m_Rot[j][1] << ", " << C.m_Poses[i].m_Rot[j][2] << "]";
                    F << "]";
                }
                F << "], \"root_positions\": [";
                for (std::size_t i = 0; i < C.m_Poses.size(); ++i) F << (i ? ", " : "") << "[" << C.m_Poses[i].m_Root[0] << ", " << C.m_Poses[i].m_Root[1] << ", " << C.m_Poses[i].m_Root[2] << "]";
                F << "]";
            }
            F << ", \"smooth_root_2d\": [";
            for (std::size_t i = 0; i < C.m_Smooth.size(); ++i) F << (i ? ", " : "") << "[" << C.m_Smooth[i][0] << ", " << C.m_Smooth[i][1] << "]";
            F << "]}" << (s + 1 < Sets.size() ? ",\n" : "\n");
        }
        F << "]\n";
        if (!F) { Error = "the constraints file could not be written"; return false; }
        return true;
    }

    // One xkimodo client run, waited on (Suggest Pose: a short window, 0.1-0.3 s warm; the first run also starts the server)
    inline bool RunBlocking(const std::wstring& Exe, const std::wstring& Args, const std::wstring& Log, DWORD TimeoutMs, std::string& Error)
    {
        std::wstring Cmd = std::format(L"\"{}\" {}", Exe, Args);
        if (const wchar_t* M = _wgetenv(L"XKIMODO_MODELS"); M && *M) Cmd += std::format(L" --models \"{}\"", M);
        SECURITY_ATTRIBUTES SA{ sizeof(SA), nullptr, TRUE };
        HANDLE hLog = CreateFileW(Log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &SA, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW SI{ sizeof(SI) };
        SI.dwFlags = STARTF_USESTDHANDLES; SI.hStdOutput = hLog; SI.hStdError = hLog; SI.hStdInput = nullptr;
        PROCESS_INFORMATION PI{};
        const BOOL Ok = CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &SI, &PI);
        if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
        if (!Ok) { Error = std::format("xkimodo could not start (error {})", GetLastError()); return false; }
        const DWORD W = WaitForSingleObject(PI.hProcess, TimeoutMs);
        DWORD Code = 1;
        if (W == WAIT_OBJECT_0) GetExitCodeProcess(PI.hProcess, &Code); else TerminateProcess(PI.hProcess, 1);
        CloseHandle(PI.hProcess); CloseHandle(PI.hThread);
        if (W != WAIT_OBJECT_0) { Error = "xkimodo did not answer in time"; return false; }
        if (Code != 0)
        {
            std::ifstream L{ std::filesystem::path(Log) }; std::stringstream S; S << L.rdbuf();
            Error = "xkimodo failed: " + S.str(); return false;
        }
        return true;
    }

    // "A person walks forward and waves." -> "WalksForwardAndWaves" (a file name from the prompt's words, without the subject)
    inline std::string NameFromPrompt(const std::string& Prompt) noexcept
    {
        std::istringstream S(Prompt); std::string W, Out; int n = 0;
        while (S >> W && n < 6)
        {
            std::string C; for (char c : W) if (std::isalnum(static_cast<unsigned char>(c))) C += c;
            if (C.empty()) continue;
            std::string Low = C; for (auto& c : Low) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (Out.empty() && (Low == "a" || Low == "the" || Low == "person" || Low == "someone" || Low == "man" || Low == "woman" || Low == "character")) continue;
            C[0] = char(std::toupper(static_cast<unsigned char>(C[0])));
            Out += C; ++n;
        }
        return Out.empty() ? "KimodoClip" : Out;
    }
}

#endif
