#include "App.hpp"
#include "gui/GUI.hpp"

#include <wx/cmdline.h>

#include <exception>
#include <algorithm>

NS_HWM_BEGIN

double const kSampleRate = 44100;
SampleCount const kBlockSize = 256;

std::shared_ptr<Sequence> MakeSequence() {
    static auto const tick_to_sample = [](int tick) -> SampleCount {
        return (SampleCount)std::round(tick / 480.0 * 0.5 * kSampleRate);
    };
    
    auto create_note = [](int tick_pos, int tick_length, UInt8 pitch, UInt8 velocity = 64, UInt8 off_velocity = 0) {
        auto sample_pos = tick_to_sample(tick_pos);
        auto sample_end_pos = tick_to_sample(tick_pos + tick_length);
        UInt8 channel = 0;
        return Sequence::Note { sample_pos, sample_end_pos - sample_pos, channel, pitch, velocity, off_velocity };
    };
    
    std::vector<Sequence::Note> notes {
        // C
        create_note(0, 1920, 48),
        create_note(0, 1920, 55),
        create_note(0, 1920, 62),
        create_note(0, 1920, 64),
        create_note(0, 1920, 67),
        create_note(0, 1920, 72),
        
        // Bb/C
        create_note(1920, 1920, 48),
        create_note(1920, 1920, 58),
        create_note(1920, 1920, 65),
        create_note(1920, 1920, 69),
        create_note(1920, 1920, 70),
        create_note(1920, 1920, 74),
        
//        create_note(480, 480, 50),
//        create_note(960, 480, 52),
//        create_note(1440, 480, 53),
//        create_note(1920, 480, 55),
//        create_note(2400, 480, 57),
//        create_note(2880, 480, 59),
//        create_note(3360, 480, 60),
    };
    
    assert(std::is_sorted(notes.begin(), notes.end(), [](auto const &lhs, auto const &rhs) {
        return lhs.pos_ < rhs.pos_;
    }));
    return std::make_shared<Sequence>(notes);
}

bool MyApp::OnInit()
{
    if(!wxApp::OnInit()) { return false; }
    
    project_ = std::make_shared<Project>();
    project_->SetSequence(MakeSequence());
    project_->GetTransporter().SetLoopRange(0, 4 * kSampleRate);
    project_->GetTransporter().SetLoopEnabled(true);
    
    adm_ = std::make_unique<AudioDeviceManager>();
    adm_->AddCallback(project_.get());
    
    auto list = adm_->Enumerate();
    for(auto const &info: list) {
        hwm::wdout << L"{} - {}({}ch)"_format(info.name_, to_wstring(info.driver_), info.num_channels_) << std::endl;
    }
    
    auto find_entry = [&list](auto io_type,
                              auto min_channels,
                              std::optional<AudioDriverType> driver = std::nullopt,
                              std::optional<String> name = std::nullopt) -> AudioDeviceInfo const *
    {
        auto found = std::find_if(list.begin(), list.end(), [&](auto const &x) {
            if(x.io_type_ != io_type)           { return false; }
            if(name && name != x.name_)         { return false; }
            if(driver && driver != x.driver_)   { return false; }
            if(x.num_channels_ < min_channels)  { return false; }
            return true;
        });
        if(found == list.end()) { return nullptr; }
        else { return &*found; }
    };

    auto output_device = find_entry(AudioDeviceIOType::kOutput, 2, adm_->GetDefaultDriver());
    if(!output_device) { output_device = find_entry(AudioDeviceIOType::kOutput, 2); }
    
    if(!output_device) {
        throw std::runtime_error("No devices found");
    }
    
    auto input_device = find_entry(AudioDeviceIOType::kInput, 2, output_device->driver_);
    
    bool const opened = adm_->Open(input_device, output_device, kSampleRate, kBlockSize);
    if(!opened) {
        throw std::runtime_error("Failed to open the device");
    }
    
    adm_->Start();
    
    MyFrame *frame = new MyFrame( "Vst3HostDemo", wxPoint(50, 50), wxSize(450, 340) );
    frame->Show( true );
    frame->SetFocus();
    frame->SetMinSize(wxSize(400, 300));
    return true;
}

int MyApp::OnExit()
{
    adm_->Close();
    project_->RemoveInstrument();
    project_.reset();
    plugin_.reset();
    factory_.reset();
    return 0;
}

void MyApp::BeforeExit()
{
    project_->RemoveInstrument();
}

void MyApp::AddFactoryLoadListener(MyApp::FactoryLoadListener *li) { fl_listeners_.AddListener(li); }
void MyApp::RemoveFactoryLoadListener(MyApp::FactoryLoadListener const *li) { fl_listeners_.RemoveListener(li); }

void MyApp::AddVst3PluginLoadListener(MyApp::Vst3PluginLoadListener *li) { vl_listeners_.AddListener(li); }
void MyApp::RemoveVst3PluginLoadListener(MyApp::Vst3PluginLoadListener const *li) { vl_listeners_.RemoveListener(li); }

bool MyApp::LoadFactory(String path)
{
    hwm::dout << "Load VST3 Module: " << path << std::endl;
    try {
        auto tmp_factory = std::make_unique<Vst3PluginFactory>(path);
        UnloadFactory();
        factory_ = std::move(tmp_factory);
        fl_listeners_.Invoke([path, this](auto *li) {
            li->OnFactoryLoaded(path, factory_.get());
        });
        return true;
    } catch(std::exception &e) {
        hwm::dout << "Create VST3 Factory failed: " << e.what() << std::endl;
        return false;
    }
}

void MyApp::UnloadFactory()
{
    if(!factory_) { return; }
    UnloadVst3Plugin(); // ロード済みのプラグインがあれば、先にアンロードしておく。
    fl_listeners_.InvokeReversed([](auto *li) { li->OnFactoryUnloaded(); });
}

bool MyApp::IsFactoryLoaded() const
{
    return !!factory_;
}

bool MyApp::LoadVst3Plugin(int component_index)
{
    assert(IsFactoryLoaded());
    
    try {
        auto tmp_plugin = factory_->CreateByIndex(component_index);
        UnloadVst3Plugin();
        plugin_ = std::move(tmp_plugin);
        project_->SetInstrument(plugin_);
        vl_listeners_.Invoke([this](auto li) {
            li->OnVst3PluginLoaded(plugin_.get());
        });
        return true;
    } catch(std::exception &e) {
        hwm::dout << "Create VST3 Plugin failed: " << e.what() << std::endl;
        return false;
    }
}

void MyApp::UnloadVst3Plugin()
{
    if(plugin_) {
        project_->RemoveInstrument();
        
        vl_listeners_.InvokeReversed([this](auto li) {
            li->OnVst3PluginUnloaded(plugin_.get());
        });
        auto tmp = std::move(plugin_);
        tmp.reset();
    }
}

bool MyApp::IsVst3PluginLoaded() const
{
    return !!plugin_;
}

Vst3PluginFactory * MyApp::GetFactory()
{
    return factory_.get();
}

Vst3Plugin * MyApp::GetPlugin()
{
    return plugin_.get();
}

Project * MyApp::GetProject()
{
    return project_.get();
}

namespace {
    wxCmdLineEntryDesc const cmdline_descs [] =
    {
        { wxCMD_LINE_SWITCH, "h", "help", "show help", wxCMD_LINE_VAL_NONE, wxCMD_LINE_OPTION_HELP },
        { wxCMD_LINE_OPTION, "d", "device", "specify device name", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
        { wxCMD_LINE_NONE },
    };
}

void MyApp::OnInitCmdLine(wxCmdLineParser& parser)
{
    parser.SetDesc(cmdline_descs);
    parser.SetSwitchChars("-");
}

bool MyApp::OnCmdLineParsed(wxCmdLineParser& parser)
{
    parser.Found(wxString("d"), &device_name_);
    return true;
}

NS_HWM_END

wxIMPLEMENT_APP(hwm::MyApp);
