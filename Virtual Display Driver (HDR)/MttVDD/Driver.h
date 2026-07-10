#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>
#include <string>

#include "Trace.h"

const int STREAM_LOG_INFO_LEN = 120;
const int monitorMaxCount = 4;//默认最多4个显示器
enum {
    ENCODER_PARAM_NOSET = 0,
    ENCODER_PARAM_RESTART = 0x0001,
    ENCODER_PARAM_RESET = 0x0002,
    ENCODER_PARAM_TEST = 0x0004,
};

enum {
    VIDEO_FRAME_FLAG_NONE = 0x00,
    VIDEO_FRAME_FLAG_RESTART = 0x01,
    VIDEO_FRAME_FLAG_IFRAME = 0x02,
};

enum {
    STREAM_LOG_INFO = 0,
    STREAM_LOG_ERR = 1,
};

#define STREAM_INFO(stream, info)                    \
	setStreamLog(stream, STREAM_LOG_INFO, info);   \
	LOG_INFO(info);

#define STREAM_ERROR(stream, info)                   \
	setStreamLog(stream, STREAM_LOG_ERR, info);    \
	LOG_ERROR(info);

LONG exception_handler(struct _EXCEPTION_POINTERS* apExceptionInfo);

struct FrameHead {
    uint8_t frame_head_len;
    uint8_t codec;
    uint8_t frame_flag;
    uint8_t resv;
    uint32_t frame_id;
    uint32_t ts;
    uint16_t width;
    uint16_t height;
};

struct DisplayInfo
{
    bool use_edid;
    bool hw_cursor;
    LONG prefer_width;
    LONG prefer_height;
    UINT prefer_fps;
    BYTE edid_data[256];
    size_t edid_len;
    SIZE size_list[256];
    size_t size_cnt;
    UINT fps_list[16];
    size_t fps_cnt;

    bool record_stream;
    int record_size;
    char record_path[256];
};

struct ShareStream
{
    // 初始化时设置
    DisplayInfo* display;
    char* stream_head;
    char* stream_config;
    char* stream_log;
    char* stream_pos;
    char* stream_buffer_common;
    char* stream_buffer;
    HANDLE shmen_handle;
    HANDLE capture_handle;
    HANDLE resume_handle;
    int stream_buffer_len;
    int max_frame_len;

    // 全局变量
    int32_t display_idx;
    int32_t check_encoder;
    int64_t write_id;
    int64_t stream_cur_pos;

    // 编码时设置
    FrameHead frame_head;
    uint64_t start_us;
    int frame_buffer_cnt;
    int frame_block_time;

    uint64_t encode_start_us;
    int force_idr; // 内部编码错误时，指示重新编码
    int resv;
};

uint64_t getUs();

HANDLE initResetEvent(void** addr);
UINT getSignDisplayCount(void* addr);

void initShareStream();
void initShareStream(UINT idx);
ShareStream* getShareStream(UINT idx);
bool checkStreamEncoder(ShareStream* stream);
void setStreamEncoder(ShareStream* stream, bool valid);
void setStreamLog(ShareStream* stream, int level, const char* info);
int waitStream(ShareStream* stream);
int pauseStream(UINT idx);


namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        struct InitChainParam {
            IDDCX_ADAPTER hAdapter;
            IDDCX_MONITOR hMonitor;
            UINT nConnectorIndex;
//            int nFramerate;
        };

//        struct TextureInfo
//        {
//            IDXGIResource* pSurface;
//            Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture2D;
//        };

        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };
        class IndirectMonitorContext;
        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IndirectMonitorContext* monitorContext,const InitChainParam &param, IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

            IndirectMonitorContext* m_monitorContext;
            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
//            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
            bool isClosed=false;
            void ClearEncoder();
        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

            InitChainParam m_InitParam;

//            int m_nTextureOk;
//            int m_nTextureCnt;
            //TextureInfo m_TextureInfo[4];
//            D3D11_TEXTURE2D_DESC m_TextureDesc;
            //IDXGIResource* m_pCurSurface = nullptr;
//            int m_nEncodeSurface;

//            EncodeParam     m_stEncodeParam;
//            EncodeConfig    m_stEncodeConfig;
//            ShareStream*    m_pShareStream;
        };

        /// <summary>
        /// Provides a sample implementation of an indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();
            void CreateMonitor(UINT displayIndex);
//            void DestroyMonitor(UINT displayIndex);
//            void DestroyMonitors();
//            void CommitModes(_In_ const IDARG_IN_COMMITMODES* pInArgs);
        protected:
            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
//            IDDCX_MONITOR m_iddMonitor[monitorMaxCount];
        };

        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(_In_ UINT ConnectorIndex, _In_ IDDCX_MONITOR Monitor, _In_ IDDCX_ADAPTER Adapter);
            virtual ~IndirectMonitorContext();

//            void CommitModes(_In_ const DISPLAYCONFIG_VIDEO_SIGNAL_INFO* pInfo);
            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

            UINT GetConnectorIndex() { return m_ConnectorIndex; }
            bool SetHwCursorMode();
        private:
       
            UINT m_ConnectorIndex;
            IDDCX_ADAPTER m_Adapter;
            IDDCX_MONITOR m_Monitor;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
            HANDLE m_hCursorEvent;
//            int m_nFrameRate = 60;
        } ;
    }
}