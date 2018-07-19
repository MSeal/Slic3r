#include "FirmwareDialog.hpp"

#include <iostream>   // XXX
#include <numeric>
#include <algorithm>
#include <boost/format.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/timer.h>
#include <wx/panel.h>
#include <wx/button.h>
#include <wx/filepicker.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/combobox.h>
#include <wx/gauge.h>
#include <wx/collpane.h>
#include <wx/msgdlg.h>

#include "libslic3r/Utils.hpp"
#include "avrdude/avrdude-slic3r.hpp"
#include "GUI.hpp"
#include "../Utils/HexFile.hpp"
#include "../Utils/Serial.hpp"

namespace fs = boost::filesystem;
namespace asio = boost::asio;
using boost::system::error_code;


namespace Slic3r {

using Utils::HexFile;


// This enum discriminates the kind of information in EVT_AVRDUDE,
// it's stored in the ExtraLong field of wxCommandEvent.
enum AvrdudeEvent
{
	AE_MESSAGE,
	AE_PROGRESS,
	AE_EXIT,
};

wxDECLARE_EVENT(EVT_AVRDUDE, wxCommandEvent);
wxDEFINE_EVENT(EVT_AVRDUDE, wxCommandEvent);


// Private

// struct HexMeta
// {
// 	static const char *hex_terminator;

// 	// Byte-offset of the second 'section' - the l10n data to be flashed into MK3 xflash
// 	// Zero if there is no second 'section'
// 	size_t lang_offset;    // FIXME: use num_secions

// 	// Holds the complete printer model/variant string to be compared with what the printer reports
// 	// Example: 1_75mm_MK3-EINSy10a-E3Dv6full
// 	std::string printer_model;

// 	HexMeta(fs::path path) : lang_offset(0)
// 	{
// 		fs::ifstream file(path);
// 		if (! file.good()) {
// 			return;
// 		}

// 		size_t offset = 0;
// 		std::string line;
// 		while (getline(file, line, '\n').good()) {
// 			// Account for LF vs CRLF
// 			if (!line.empty() && line.back() != '\r') {
// 				line.push_back('\r');
// 			}

// 			if (line == hex_terminator) {
// 				if (offset == 0) {
// 					// This is the first terminator seen, save the position
// 					offset = file.tellg();
// 				} else {
// 					// We've found another terminator, use the offset just after the first one
// 					// which is the start of the second 'section'.
// 					this->lang_offset = offset;
// 				}
// 			}
// 		}

// 		if (!line.empty() && line[0] != ':') {
// 			// The printer model string is on the last line, if any
// 			line.pop_back();   // Remove the '\r' we added earlier
// 			this->printer_model = std::move(line);
// 		}
// 	}
// };

// const char *HexMeta::hex_terminator = ":00000001FF\r";

struct FirmwareDialog::priv
{
	enum AvrDudeComplete
	{
		AC_NONE,
		AC_SUCCESS,
		AC_FAILURE,
		AC_CANCEL,
	};

	FirmwareDialog *q;      // PIMPL back pointer ("Q-Pointer")

	wxComboBox *port_picker;
	std::vector<Utils::SerialPortInfo> ports;
	wxFilePickerCtrl *hex_picker;
	wxStaticText *txt_status;
	wxGauge *progressbar;
	wxCollapsiblePane *spoiler;
	wxTextCtrl *txt_stdout;
	wxButton *btn_rescan;
	wxButton *btn_close;
	wxButton *btn_flash;
	wxString btn_flash_label_ready;
	wxString btn_flash_label_flashing;

	wxTimer timer_pulse;

	// This is a shared pointer holding the background AvrDude task
	// also serves as a status indication (it is set _iff_ the background task is running, otherwise it is reset).
	AvrDude::Ptr avrdude;
	std::string avrdude_config;
	unsigned progress_tasks_done;
	unsigned progress_tasks_bar;
	bool cancelled;
	const bool extra_verbose;   // For debugging

	priv(FirmwareDialog *q) :
		q(q),
		btn_flash_label_ready(_(L("Flash!"))),
		btn_flash_label_flashing(_(L("Cancel"))),
		timer_pulse(q),
		avrdude_config((fs::path(::Slic3r::resources_dir()) / "avrdude" / "avrdude.conf").string()),
		progress_tasks_done(0),
		progress_tasks_bar(0),
		cancelled(false),
		extra_verbose(false)
	{}

	void find_serial_ports();
	void flashing_start(unsigned tasks);
	void flashing_done(AvrDudeComplete complete);
	// size_t hex_num_sections(const wxString &path);
	bool check_model_id(const HexFile &metadata, std::string port);

	void prepare_common(AvrDude &, const std::string &port);
	void prepare_mk2(AvrDude &, const std::string &port);
	void prepare_mk3(AvrDude &, const std::string &port);
	void prepare_mk3_mmu(AvrDude &, const std::string &port);
	void perform_upload();

	void cancel();
	void on_avrdude(const wxCommandEvent &evt);
};

void FirmwareDialog::priv::find_serial_ports()
{
	auto new_ports = Utils::scan_serial_ports_extended();
	if (new_ports != this->ports) {
		this->ports = new_ports;
		port_picker->Clear();
		for (const auto &port : this->ports)
			port_picker->Append(port.friendly_name);
		if (ports.size() > 0) {
			int idx = port_picker->GetValue().IsEmpty() ? 0 : -1;
			for (int i = 0; i < (int)this->ports.size(); ++ i)
				if (this->ports[i].is_printer) {
					idx = i;
					break;
				}
			if (idx != -1)
				port_picker->SetSelection(idx);
		}
	}
}

void FirmwareDialog::priv::flashing_start(unsigned tasks)
{
	txt_stdout->Clear();
	txt_status->SetLabel(_(L("Flashing in progress. Please do not disconnect the printer!")));
	txt_status->SetForegroundColour(GUI::get_label_clr_modified());
	port_picker->Disable();
	btn_rescan->Disable();
	hex_picker->Disable();
	btn_close->Disable();
	btn_flash->SetLabel(btn_flash_label_flashing);
	progressbar->SetRange(200 * tasks);   // See progress callback below
	progressbar->SetValue(0);
	progress_tasks_done = 0;
	progress_tasks_bar = 0;
	cancelled = false;
	timer_pulse.Start(50);
}

void FirmwareDialog::priv::flashing_done(AvrDudeComplete complete)
{
	auto text_color = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	port_picker->Enable();
	btn_rescan->Enable();
	hex_picker->Enable();
	btn_close->Enable();
	btn_flash->SetLabel(btn_flash_label_ready);
	txt_status->SetForegroundColour(text_color);
	timer_pulse.Stop();
	progressbar->SetValue(progressbar->GetRange());

	switch (complete) {
	case AC_SUCCESS: txt_status->SetLabel(_(L("Flashing succeeded!"))); break;
	case AC_FAILURE: txt_status->SetLabel(_(L("Flashing failed. Please see the avrdude log below."))); break;
	case AC_CANCEL: txt_status->SetLabel(_(L("Flashing cancelled."))); break;
	}
}

// size_t FirmwareDialog::priv::hex_num_sections(const wxString &path)   // XXX: remove in favor of HexFile
// {
// 	fs::ifstream file(fs::path(path.wx_str()));
// 	if (! file.good()) {
// 		return 0;
// 	}

// 	static const char *hex_terminator = ":00000001FF\r";
// 	size_t res = 0;
// 	std::string line;
// 	while (getline(file, line, '\n').good()) {
// 		// Account for LF vs CRLF
// 		if (!line.empty() && line.back() != '\r') {
// 			line.push_back('\r');
// 		}

// 		if (line == hex_terminator) {
// 			res++;
// 		}
// 	}

// 	return res;
// }

bool FirmwareDialog::priv::check_model_id(const HexFile &metadata, std::string port)
{
	if (metadata.model_id.empty()) {
		return false;
	}

	asio::io_service io;
	Utils::Serial serial(io, port, 115200);

	enum {
		TIMEOUT = 1000,
		RETREIES = 3,
	};

	if (! serial.printer_ready_wait(RETREIES, TIMEOUT)) {
		return false;
	}

	std::string line;
	error_code ec;
	for (unsigned retries = RETREIES; retries > 0; retries--) {
		serial.printer_write_line("PRUSA Rev");

		while (serial.read_line(TIMEOUT, line, ec)) {
			if (line == metadata.model_id) {
				return true;
			}
			line.clear();
		}
	}

	return false;
}

void FirmwareDialog::priv::prepare_common(AvrDude &avrdude, const std::string &port)
{
	auto filename = hex_picker->GetPath();

	std::vector<std::string> args {{
		extra_verbose ? "-vvvvv" : "-v",
		"-p", "atmega2560",
		// Using the "Wiring" mode to program Rambo or Einsy, using the STK500v2 protocol (not the STK500).
		// The Prusa's avrdude is patched to never send semicolons inside the data packets, as the USB to serial chip
		// is flashed with a buggy firmware.
		"-c", "wiring",
		"-P", port,
		"-b", "115200",   // TODO: Allow other rates? Ditto below.
		"-D",
		"-U", (boost::format("flash:w:0:%1%:i") % filename.utf8_str().data()).str(),
	}};

	BOOST_LOG_TRIVIAL(info) << "Invoking avrdude, arguments: "
		<< std::accumulate(std::next(args.begin()), args.end(), args[0], [](std::string a, const std::string &b) {
			return a + ' ' + b;
		});

	avrdude.push_args(std::move(args));
}

void FirmwareDialog::priv::prepare_mk2(AvrDude &avrdude, const std::string &port)
{
	flashing_start(1);
	prepare_common(avrdude, port);
}

void FirmwareDialog::priv::prepare_mk3(AvrDude &avrdude, const std::string &port)
{
	flashing_start(2);
	prepare_common(avrdude, port);

	auto filename = hex_picker->GetPath();

	// The hex file also contains another section with l10n data to be flashed into the external flash on MK3 (Einsy)
	// This is done via another avrdude invocation, here we build arg list for that:
	std::vector<std::string> args_l10n {{
		extra_verbose ? "-vvvvv" : "-v",
		"-p", "atmega2560",
		// Using the "Arduino" mode to program Einsy's external flash with languages, using the STK500 protocol (not the STK500v2).
		// The Prusa's avrdude is patched again to never send semicolons inside the data packets.
		"-c", "arduino",
		"-P", port,
		"-b", "115200",
		"-D",
		"-u", // disable safe mode
		"-U", (boost::format("flash:w:1:%1%:i") % filename.utf8_str().data()).str(),
	}};

	BOOST_LOG_TRIVIAL(info) << "Invoking avrdude for external flash flashing, arguments: "
		<< std::accumulate(std::next(args_l10n.begin()), args_l10n.end(), args_l10n[0], [](std::string a, const std::string &b) {
			return a + ' ' + b;
		});

	avrdude.push_args(std::move(args_l10n));
}

void FirmwareDialog::priv::prepare_mk3_mmu(AvrDude &avrdude, const std::string &port)
{
	// TODO
}


void FirmwareDialog::priv::perform_upload()
{
	auto filename = hex_picker->GetPath();
	std::string port = port_picker->GetValue().ToStdString();  // XXX: Do this here? What about MMU?
	int selection = port_picker->GetSelection();
	if (selection != -1) {
		// Verify whether the combo box list selection equals to the combo box edit value.
		if (this->ports[selection].friendly_name == port)
			port = this->ports[selection].port;
	}
	// if (filename.IsEmpty() || port.empty()) { return; }     // XXX

	const bool extra_verbose = false;   // For debugging
	// const auto num_secions = hex_num_sections(filename);
	HexFile metadata(filename.wx_str());
	const auto filename_utf8 = filename.utf8_str();

	// Init the avrdude object
	AvrDude avrdude(avrdude_config);

	switch (metadata.device) {
		case HexFile::DEV_GENERIC:
		case HexFile::DEV_MK2:
		default:
			prepare_mk2(avrdude, port);
		break;

		case HexFile::DEV_MK3:
			prepare_mk3(avrdude, port);
		break;

		case HexFile::DEV_MK3_MMU:
			prepare_mk3_mmu(avrdude, port);
		break;
	}

	// // Build argument list(s)
	// std::vector<std::string> args {{
	// 	extra_verbose ? "-vvvvv" : "-v",
	// 	"-p", "atmega2560",
	// 	// Using the "Wiring" mode to program Rambo or Einsy, using the STK500v2 protocol (not the STK500).
	// 	// The Prusa's avrdude is patched to never send semicolons inside the data packets, as the USB to serial chip
	// 	// is flashed with a buggy firmware.
	// 	"-c", "wiring",
	// 	"-P", port,
	// 	"-b", "115200",   // TODO: Allow other rates? Ditto below.
	// 	"-D",
	// 	// XXX: Safe mode?
	// 	"-U", (boost::format("flash:w:0:%1%:i") % filename_utf8.data()).str(),
	// }};

	// BOOST_LOG_TRIVIAL(info) << "Invoking avrdude, arguments: "
	// 	<< std::accumulate(std::next(args.begin()), args.end(), args[0], [](std::string a, const std::string &b) {
	// 		return a + ' ' + b;
	// 	});

	// avrdude.push_args(std::move(args));

	// if (num_secions > 1) {
	// 	// The hex file also contains another section with l10n data to be flashed into the external flash on MK3 (Einsy)
	// 	// This is done via another avrdude invocation, here we build arg list for that:
	// 	std::vector<std::string> args_l10n {{
	// 		extra_verbose ? "-vvvvv" : "-v",
	// 		"-p", "atmega2560",
	// 		// Using the "Arduino" mode to program Einsy's external flash with languages, using the STK500 protocol (not the STK500v2).
	// 		// The Prusa's avrdude is patched again to never send semicolons inside the data packets.
	// 		"-c", "arduino",
	// 		"-P", port,
	// 		"-b", "115200",
	// 		"-D",
	// 		"-u", // disable safe mode
	// 		"-U", (boost::format("flash:w:1:%1%:i") % filename_utf8.data()).str(),
	// 	}};

	// 	BOOST_LOG_TRIVIAL(info) << "Invoking avrdude for external flash flashing, arguments: "
	// 		<< std::accumulate(std::next(args_l10n.begin()), args_l10n.end(), args_l10n[0], [](std::string a, const std::string &b) {
	// 			return a + ' ' + b;
	// 		});

	// 	avrdude.push_args(std::move(args_l10n));
	// }

	// It is ok here to use the q-pointer to the FirmwareDialog
	// because the dialog ensures it doesn't exit before the background thread is done.
	auto q = this->q;

	this->avrdude = avrdude
		.on_run([this, metadata, port](bool &cancel) {
			// TODO

			// const auto model_matches = this->check_model_id(metadata, std::move(port));
			// std::cerr << "check_model_id: " << model_matches << std::endl;

			// this->cancelled = true;
			// cancel = true;
		})
		.on_message(std::move([q, extra_verbose](const char *msg, unsigned /* size */) {
			if (extra_verbose) {
				BOOST_LOG_TRIVIAL(debug) << "avrdude: " << msg;
			}

			auto evt = new wxCommandEvent(EVT_AVRDUDE, q->GetId());
			auto wxmsg = wxString::FromUTF8(msg);
			evt->SetExtraLong(AE_MESSAGE);
			evt->SetString(std::move(wxmsg));
			wxQueueEvent(q, evt);
		}))
		.on_progress(std::move([q](const char * /* task */, unsigned progress) {
			auto evt = new wxCommandEvent(EVT_AVRDUDE, q->GetId());
			evt->SetExtraLong(AE_PROGRESS);
			evt->SetInt(progress);
			wxQueueEvent(q, evt);
		}))
		.on_complete(std::move([q](int status, size_t /* args_id */) {
			auto evt = new wxCommandEvent(EVT_AVRDUDE, q->GetId());
			evt->SetExtraLong(AE_EXIT);
			evt->SetInt(status);
			wxQueueEvent(q, evt);
		}))
		.run();
}

void FirmwareDialog::priv::cancel()
{
	if (avrdude) {
		cancelled = true;
		txt_status->SetLabel(_(L("Cancelling...")));
		avrdude->cancel();
	}
}

void FirmwareDialog::priv::on_avrdude(const wxCommandEvent &evt)
{
	AvrDudeComplete complete_kind;

	switch (evt.GetExtraLong()) {
	case AE_MESSAGE:
		txt_stdout->AppendText(evt.GetString());
		break;

	case AE_PROGRESS:
		// We try to track overall progress here.
		// Avrdude performs 3 tasks per one memory operation ("-U" arg),
		// first of which is reading of status data (very short).
		// We use the timer_pulse during the very first task to indicate intialization
		// and then display overall progress during the latter tasks.

		if (progress_tasks_done > 0) {
			progressbar->SetValue(progress_tasks_bar + evt.GetInt());
		}

		if (evt.GetInt() == 100) {
			timer_pulse.Stop();
			if (progress_tasks_done % 3 != 0) {
				progress_tasks_bar += 100;
			}
			progress_tasks_done++;
		}

		break;

	case AE_EXIT:
		BOOST_LOG_TRIVIAL(info) << "avrdude exit code: " << evt.GetInt();

		complete_kind = cancelled ? AC_CANCEL : (evt.GetInt() == 0 ? AC_SUCCESS : AC_FAILURE);
		flashing_done(complete_kind);

		// Make sure the background thread is collected and the AvrDude object reset
		if (avrdude) { avrdude->join(); }
		avrdude.reset();

		break;

	default:
		break;
	}
}


// Public

FirmwareDialog::FirmwareDialog(wxWindow *parent) :
	wxDialog(parent, wxID_ANY, _(L("Firmware flasher")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
	p(new priv(this))
{
	enum {
		DIALOG_MARGIN = 15,
		SPACING = 10,
		MIN_WIDTH = 600,
		MIN_HEIGHT = 200,
		MIN_HEIGHT_EXPANDED = 500,
	};

	wxFont status_font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	status_font.MakeBold();
	wxFont mono_font(wxFontInfo().Family(wxFONTFAMILY_TELETYPE));
	mono_font.MakeSmaller();

	auto *panel = new wxPanel(this);
	wxBoxSizer *vsizer = new wxBoxSizer(wxVERTICAL);
	panel->SetSizer(vsizer);

	auto *label_port_picker = new wxStaticText(panel, wxID_ANY, _(L("Serial port:")));
	p->port_picker = new wxComboBox(panel, wxID_ANY);
	p->btn_rescan = new wxButton(panel, wxID_ANY, _(L("Rescan")));
	auto *port_sizer = new wxBoxSizer(wxHORIZONTAL);
	port_sizer->Add(p->port_picker, 1, wxEXPAND | wxRIGHT, SPACING);
	port_sizer->Add(p->btn_rescan, 0);

	auto *label_hex_picker = new wxStaticText(panel, wxID_ANY, _(L("Firmware image:")));
	p->hex_picker = new wxFilePickerCtrl(panel, wxID_ANY);

	auto *label_status = new wxStaticText(panel, wxID_ANY, _(L("Status:")));
	p->txt_status = new wxStaticText(panel, wxID_ANY, _(L("Ready")));
	p->txt_status->SetFont(status_font);

	auto *label_progress = new wxStaticText(panel, wxID_ANY, _(L("Progress:")));
	p->progressbar = new wxGauge(panel, wxID_ANY, 1, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL | wxGA_SMOOTH);

	auto *grid = new wxFlexGridSizer(2, SPACING, SPACING);
	grid->AddGrowableCol(1);
	grid->Add(label_port_picker, 0, wxALIGN_CENTER_VERTICAL);
	grid->Add(port_sizer, 0, wxEXPAND);

	grid->Add(label_hex_picker, 0, wxALIGN_CENTER_VERTICAL);
	grid->Add(p->hex_picker, 0, wxEXPAND);

	grid->Add(label_status, 0, wxALIGN_CENTER_VERTICAL);
	grid->Add(p->txt_status, 0, wxEXPAND);

	grid->Add(label_progress, 0, wxALIGN_CENTER_VERTICAL);
	grid->Add(p->progressbar, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

	vsizer->Add(grid, 0, wxEXPAND | wxTOP | wxBOTTOM, SPACING);

	p->spoiler = new wxCollapsiblePane(panel, wxID_ANY, _(L("Advanced: avrdude output log")));
	auto *spoiler_pane = p->spoiler->GetPane();
	auto *spoiler_sizer = new wxBoxSizer(wxVERTICAL);
	p->txt_stdout = new wxTextCtrl(spoiler_pane, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	p->txt_stdout->SetFont(mono_font);
	spoiler_sizer->Add(p->txt_stdout, 1, wxEXPAND);
	spoiler_pane->SetSizer(spoiler_sizer);
	// The doc says proportion need to be 0 for wxCollapsiblePane.
	// Experience says it needs to be 1, otherwise things won't get sized properly.
	vsizer->Add(p->spoiler, 1, wxEXPAND | wxBOTTOM, SPACING);

	p->btn_close = new wxButton(panel, wxID_CLOSE);
	p->btn_flash = new wxButton(panel, wxID_ANY, p->btn_flash_label_ready);
	auto *bsizer = new wxBoxSizer(wxHORIZONTAL);
	bsizer->Add(p->btn_close);
	bsizer->AddStretchSpacer();
	bsizer->Add(p->btn_flash);
	vsizer->Add(bsizer, 0, wxEXPAND);

	auto *topsizer = new wxBoxSizer(wxVERTICAL);
	topsizer->Add(panel, 1, wxEXPAND | wxALL, DIALOG_MARGIN);
	SetMinSize(wxSize(MIN_WIDTH, MIN_HEIGHT));
	SetSizerAndFit(topsizer);
	const auto size = GetSize();
	SetSize(std::max(size.GetWidth(), static_cast<int>(MIN_WIDTH)), std::max(size.GetHeight(), static_cast<int>(MIN_HEIGHT)));
	Layout();

	p->spoiler->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &evt) {
		// Dialog size gets screwed up by wxCollapsiblePane, we need to fix it here
		if (evt.GetCollapsed()) {
			this->SetMinSize(wxSize(MIN_WIDTH, MIN_HEIGHT));
		} else {
			this->SetMinSize(wxSize(MIN_WIDTH, MIN_HEIGHT_EXPANDED));
		}

		this->Fit();
		this->Layout();
	});

	p->btn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->Close(); });
	p->btn_rescan->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->p->find_serial_ports(); });

	p->btn_flash->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
		if (this->p->avrdude) {
			// Flashing is in progress, ask the user if they're really sure about canceling it
			wxMessageDialog dlg(this,
				_(L("Are you sure you want to cancel firmware flashing?\nThis could leave your printer in an unusable state!")),
				_(L("Confirmation")),
				wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
			if (dlg.ShowModal() == wxID_YES) {
				this->p->cancel();
			}
		} else {
			// Start a flashing task
			this->p->perform_upload();
		}
	});

	Bind(wxEVT_TIMER, [this](wxTimerEvent &evt) { this->p->progressbar->Pulse(); });

	Bind(EVT_AVRDUDE, [this](wxCommandEvent &evt) { this->p->on_avrdude(evt); });

	Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &evt) {
		if (this->p->avrdude) {
			evt.Veto();
		} else {
			evt.Skip();
		}
	});

	p->find_serial_ports();
}

FirmwareDialog::~FirmwareDialog()
{
	// Needed bacuse of forward defs
}

void FirmwareDialog::run(wxWindow *parent)
{
	FirmwareDialog dialog(parent);
	dialog.ShowModal();
}


}
