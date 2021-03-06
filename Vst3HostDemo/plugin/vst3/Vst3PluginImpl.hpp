#include "Vst3Plugin.hpp"

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <atomic>
#include <vector>

#include <pluginterfaces/base/ftypes.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/base/ustring.h>
#include <pluginterfaces/vst/vstpresetkeys.h>

#include "Vst3Utils.hpp"
#include "Vst3Plugin.hpp"
#include "Vst3PluginFactory.hpp"

#include "../../misc/Flag.hpp"
#include "../../misc/Buffer.hpp"

NS_HWM_BEGIN

using namespace Steinberg;

class Vst3Plugin::Impl
{
public:
	typedef Impl this_type;

	using component_ptr_t           = vstma_unique_ptr<Vst::IComponent>;
	using audio_processor_ptr_t     = vstma_unique_ptr<Vst::IAudioProcessor>;
	using edit_controller_ptr_t     = vstma_unique_ptr<Vst::IEditController>;
	using edit_controller2_ptr_t    = vstma_unique_ptr<Vst::IEditController2>;
	using parameter_changes_ptr_t   = vstma_unique_ptr<Vst::IParameterChanges>;
	using plug_view_ptr_t           = vstma_unique_ptr<IPlugView>;
	using unit_info_ptr_t           = vstma_unique_ptr<Vst::IUnitInfo>;
	using program_list_data_ptr_t   = vstma_unique_ptr<Vst::IProgramListData>;

	enum ErrorContext {
		kFactoryError,
		kComponentError,
		kAudioProcessorError,
		kEditControllerError,
		kEditController2Error
	};

	enum class Status {
		kInvalid,
		kCreated,
		kInitialized,
		kSetupDone,
		kActivated,
		kProcessing,
	};

	class Error
    :	public std::runtime_error
	{
    public:
		Error(ErrorContext error_context, tresult error_code)
			:	std::runtime_error("VstPlugin::Error")
			,	error_context_(error_context)
			,	error_code_(error_code)
		{}

		ErrorContext context() { return error_context_; }
		tresult code() { return error_code_; }

	private:
		tresult			error_code_;
		ErrorContext	error_context_;
	};
    
    using ParameterInfoList = IdentifiedValueList<ParameterInfo>;
    using UnitInfoList = IdentifiedValueList<UnitInfo>;
    
    struct AudioBusesInfo
    {
        void Initialize(Impl *owner, Vst::BusDirection dir);
        
        size_t GetNumBuses() const;
        
        BusInfo const & GetBusInfo(UInt32 bus_index) const;
        
        //! すべてのバスのチャンネル数の総計
        //! これは、各バスのSpeakerArrangement状態によって変化する。
        //! バスのアクティブ状態には影響を受けない。
        //!  (つまり、各バスがアクティブかそうでないかに関わらず、すべてのバスのチャンネルが足し合わされる。)
        size_t GetNumChannels() const;

        //! すべてのアクティブなバスのチャンネル数の総計
        //! これは、各バスのアクティブ状態やSpeakerArrangement状態によって変化する。
        size_t GetNumActiveChannels() const;
        
        bool IsActive(size_t bus_index) const;
        void SetActive(size_t bus_index, bool state = true);
        
        //! @return true if this speaker arrangement is accepted to the plugin successfully,
        //! false otherwise.
        bool SetSpeakerArrangement(size_t bus_index, Vst::SpeakerArrangement arr);
        
        Vst::AudioBusBuffers * GetBusBuffers();
        
    private:
        Impl *owner_ = nullptr;
        std::vector<BusInfo> bus_infos_;
        Vst::BusDirection dir_;
        
        //! bus_infos_のis_active_状態によらず、定義されているすべてのバスと同じ数だけ用意される。
        std::vector<Vst::AudioBusBuffers> bus_buffers_;
        
        void UpdateBusBuffers();
    };

public:
	Impl(IPluginFactory *factory,
         ClassInfo const &info,
         FUnknown *host_context);

    ~Impl();

	bool HasEditController	() const;
	bool HasEditController2	() const;

	Vst::IComponent	*		GetComponent		();
	Vst::IAudioProcessor *	GetAudioProcessor	();
	Vst::IEditController *	GetEditController	();
	Vst::IEditController2 *	GetEditController2	();
	Vst::IEditController *	GetEditController	() const;
	Vst::IEditController2 *	GetEditController2	() const;

	String GetEffectName() const;
    
    ParameterInfoList & GetParameterInfoList();
    ParameterInfoList const & GetParameterInfoList() const;
    
    UnitInfoList & GetUnitInfoList();
    UnitInfoList const & GetUnitInfoList() const;
    
    AudioBusesInfo & GetBusesInfo(BusDirection dir);
    AudioBusesInfo const & GetBusesInfo(BusDirection dir) const;
    
    UInt32 GetNumParameters() const;
    Vst::ParamValue GetParameterValueByIndex(UInt32 index) const;
    Vst::ParamValue GetParameterValueByID(Vst::ParamID id) const;
    
    UInt32  GetProgramIndex(Vst::UnitID unit_id = 0) const;
    void    SetProgramIndex(UInt32 index, Vst::UnitID unit_id = 0);

	bool HasEditor() const;

	bool OpenEditor(WindowHandle parent, IPlugFrame *plug_frame);

	void CloseEditor();

	bool IsEditorOpened() const;

	ViewRect GetPreferredRect() const;

	void Resume();

	void Suspend();

	bool IsResumed() const;

	void SetBlockSize(int block_size);

	void SetSamplingRate(int sampling_rate);

	void	RestartComponent(Steinberg::int32 flags);

	void    Process(ProcessInfo pi);

//! Parameter Change
public:
	//! PopFrontParameterChangesとの呼び出しはスレッドセーフ
	void PushBackParameterChange(Vst::ParamID id, Vst::ParamValue value);
    
private:
    //! PushBackParameterChangeとの呼び出しはスレッドセーフ
    void PopFrontParameterChanges(Vst::ParameterChanges &dest);

private:
	void LoadPlugin(IPluginFactory *factory, ClassInfo const &info, FUnknown *host_context);
	void LoadInterfaces(IPluginFactory *factory, ClassInfo const &info, FUnknown *host_context);
	void Initialize(vstma_unique_ptr<Vst::IComponentHandler> component_handler);

	tresult CreatePlugView();
	void DeletePlugView();

	void PrepareParameters();
	void PrepareUnitInfo();

	void UnloadPlugin();

private:
    std::optional<ClassInfo> plugin_info_;
	component_ptr_t			component_;
	audio_processor_ptr_t	audio_processor_;
	edit_controller_ptr_t	edit_controller_;
	edit_controller2_ptr_t	edit_controller2_;
	plug_view_ptr_t			plug_view_;
	unit_info_ptr_t			unit_handler_;
    UnitInfoList            unit_info_list_;
    ParameterInfoList       parameter_info_list_;

	Flag					is_processing_started_;
	Flag					edit_controller_is_created_new_;
	Flag					has_editor_;
	Flag					is_editor_opened_;
	Flag					is_resumed_;
	Flag					param_value_changes_was_specified_;

	int	sampling_rate_;
	int block_size_;
    
    void UpdateBusBuffers();
    
    AudioBusesInfo input_buses_info_;
    AudioBusesInfo output_buses_info_;
    
    // Vst3Plugin側にバッファを持たせないで、外側にあるバッファを使い回すほうが、コピーの手間が減っていいが、
    // ちょっと設計がややこしくなるので、いまはここにバッファを持たせるようにしておく。
    Buffer<float> input_buffer_;
    Buffer<float> output_buffer_;
    
    Status status_;
    
private:
    std::mutex              parameter_queue_mutex_;
    Vst::ParameterChanges   param_changes_queue_;
    
    Vst::ParameterChanges input_params_;
    Vst::ParameterChanges output_params_;
    Vst::EventList input_events_;
    Vst::EventList output_events_;
};

NS_HWM_END
