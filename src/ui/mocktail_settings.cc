// Modified by vii from komaruworld/mocktail. See README "About this fork".
//
// A small GTK4 window for the settings that are worth changing without opening
// a text editor. It reads and writes config.ui.yaml, never config.yaml, so the
// hand-edited file and its comments are left exactly as they are. See
// include/runtime/ui_config_overrides.h for why.

#include <adwaita.h>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"
#include "runtime/sibling_executable.h"
#include "runtime/ui_config_overrides.h"

namespace {

using mocktail::runtime::UiConfigField;
using mocktail::runtime::UiConfigOverrides;


constexpr char kApplicationId[] = "io.github.CoderDayton.nightcap";

// One control per field, so a change can be read back and the "set here" state
// shown. The raw pointer is owned by its container, which outlives the row.
struct FieldRow {
  const UiConfigField* field = nullptr;
  GtkWidget* container = nullptr;
  // Exactly one of these is set, depending on the field's kind.
  GtkWidget* control = nullptr;
  GtkWidget* reset_button = nullptr;
  // The list behind a drop-down, held explicitly so it cannot be collected
  // while the drop-down still asks it questions. Relying on the drop-down's own
  // reference turned out not to be enough: the model was already gone by the
  // time the initial value was set, and every drop-down raised a list-model
  // assertion on open.
  GtkStringList* option_model = nullptr;
  // The value the field had when the window opened, so Cancel can put it back.
  std::string original;
  // Whether the window is the reason this key is set, as opposed to
  // config.yaml or the environment.
  bool set_by_window = false;
  // Where the effective value came from, shown as help text under the control.
  std::string source = "default";
};

struct Window {
  AdwApplicationWindow* window = nullptr;
  AdwToastOverlay* toasts = nullptr;
  GtkWidget* status_label = nullptr;
  GtkWidget* restart_button = nullptr;
  // True while the controls are being given their initial values. Their change
  // signals then mean nothing, because the user has not touched anything yet.
  bool initializing = false;
  // True once the window is closing. GTK emits property notifications while it
  // tears widgets down, so a control's change signal can arrive after the
  // widget it names is already being destroyed. Following the pointer then
  // reads freed memory, which GTK catches as a list-model assertion and which
  // turns into a crash.
  bool tearing_down = false;
  std::vector<FieldRow*> rows;
  // Owns the field descriptions the rows point at. UiConfigFields() returns by
  // value, so a row holding a pointer into that temporary would dangle the
  // moment the loop that produced it ended.
  std::vector<UiConfigField> fields;
  UiConfigOverrides overrides;
  std::filesystem::path config_ui_file;
  std::string config_yaml_file;
};

Window* g_window = nullptr;


// The environment beats every file, but the window is a separate process from
// the app and cannot know what the app's environment will hold at launch. So
// it does not pretend to lock controls it cannot reliably identify: the banner
// states the order instead, and a key the environment does supply simply wins.

std::string CurrentValue(FieldRow* row) {
  // A control's change signal can arrive while the window is being destroyed,
  // and reading the control then is a use-after-free rather than a value.
  if (g_window != nullptr && g_window->tearing_down) {
    return row->field->default_value;
  }
  if (row->field->boolean) {
    return gtk_switch_get_active(GTK_SWITCH(row->control)) ? "true" : "false";
  }
  if (row->field->numeric) {
    return std::to_string(
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(row->control)));
  }
  if (row->field->free_text) {
    const gchar* text = gtk_editable_get_text(GTK_EDITABLE(row->control));
    return text != nullptr ? std::string(text) : std::string();
  }
  // gtk_drop_down_get_selected returns an index, so the value is read back out
  // of the model rather than returned directly.
  const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(row->control));
  if (selected == GTK_INVALID_LIST_POSITION) {
    return row->field->default_value;
  }
  GListModel* model = gtk_drop_down_get_model(GTK_DROP_DOWN(row->control));
  if (model == nullptr) {
    return row->field->default_value;
  }
  gpointer item = g_list_model_get_item(model, selected);
  if (item == nullptr) {
    return row->field->default_value;
  }
  std::string value;
  if (GTK_IS_STRING_LIST(model)) {
    const gchar* text = gtk_string_list_get_string(GTK_STRING_LIST(model),
                                                   static_cast<guint>(selected));
    if (text != nullptr) {
      value = text;
    }
  }
  g_object_unref(item);
  return value.empty() ? row->field->default_value : value;
}

void SetControlValue(FieldRow* row, const std::string& value) {
  if (row->field->boolean) {
    gtk_switch_set_active(GTK_SWITCH(row->control), value == "true");
    return;
  }
  if (row->field->numeric) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->control),
                              std::stod(value.empty() ? "0" : value));
    return;
  }
  if (row->field->free_text) {
    gtk_editable_set_text(GTK_EDITABLE(row->control), value.c_str());
    return;
  }
  for (guint index = 0; index < row->field->allowed_values.size(); ++index) {
    if (row->field->allowed_values[index] == value) {
      gtk_drop_down_set_selected(GTK_DROP_DOWN(row->control), index);
      return;
    }
  }
}

void ShowToast(const std::string& message) {
  if (g_window == nullptr || g_window->toasts == nullptr) {
    return;
  }
  AdwToast* toast = adw_toast_new(message.c_str());
  adw_toast_set_timeout(toast, 4);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(g_window->toasts), toast);
}

void UpdateRowState(FieldRow* row);

// Recomputes every row's "set here" marker and reset button. Called after any
// change so the window always shows which values it is responsible for.
void RefreshAllRows() {
  if (g_window == nullptr) {
    return;
  }
  for (FieldRow* row : g_window->rows) {
    UpdateRowState(row);
  }
}

void UpdateRowState(FieldRow* row) {
  const std::string current = CurrentValue(row);
  const bool differs = current != row->field->default_value;
  const std::string* stored = g_window->overrides.Find(row->field->key);
  row->set_by_window = stored != nullptr;

  // A control is only worth showing as changed when the window has actually
  // set it, which is what the reset button reveals.
  if (row->reset_button != nullptr) {
    gtk_widget_set_visible(row->reset_button, row->set_by_window);
  }
  gtk_widget_set_sensitive(row->container, TRUE);
  (void)differs;
}

void OnFieldChanged(GtkWidget* control, gpointer user_data) {
  (void)control;
  auto* row = static_cast<FieldRow*>(user_data);
  if (g_window == nullptr || row->field == nullptr) {
    return;
  }
  // Setting a control's initial value emits its change signal. Treating that
  // as an edit would leave the window claiming unsaved changes before the user
  // has touched it, and would mark a field as set when the user never chose it.
  if (g_window->initializing) {
    return;
  }
  // The same signal arrives while the window is being torn down, when the
  // control may already be destroyed. Reading it then is a use-after-free.
  if (g_window->tearing_down) {
    return;
  }
  const std::string value = CurrentValue(row);
  std::string error;
  if (!mocktail::runtime::ValidateUiConfigValue(row->field->key, value, &error)) {
    // Should not happen, because the control constrains the value, but a
    // rejected value must never reach the file.
    ShowToast(error);
    return;
  }
  if (value == row->field->default_value) {
    g_window->overrides.Clear(row->field->key);
  } else {
    g_window->overrides.Set(row->field->key, value);
  }
  UpdateRowState(row);
  if (g_window->status_label != nullptr) {
    gtk_label_set_text(GTK_LABEL(g_window->status_label),
                       "Unsaved changes. They apply on the next launch.");
  }
}

void OnResetClicked(GtkWidget* button, gpointer user_data) {
  (void)button;
  auto* row = static_cast<FieldRow*>(user_data);
  if (g_window == nullptr || row->field == nullptr) {
    return;
  }
  g_window->overrides.Clear(row->field->key);
  // A free-text field is not constrained, so reset is the only way back to the
  // default and the control has to be told.
  SetControlValue(row, row->field->default_value);
  UpdateRowState(row);
  if (g_window->status_label != nullptr) {
    gtk_label_set_text(GTK_LABEL(g_window->status_label),
                       "Unsaved changes. They apply on the next launch.");
  }
}

// Reads the current values into the override set and writes the file. Returns
// whether it succeeded, and reports why through the status label.
bool SaveChanges() {
  if (g_window == nullptr) {
    return false;
  }
  for (FieldRow* row : g_window->rows) {
    const std::string value = CurrentValue(row);
    std::string error;
    if (!mocktail::runtime::ValidateUiConfigValue(row->field->key, value, &error)) {
      if (g_window->status_label != nullptr) {
        gtk_label_set_text(GTK_LABEL(g_window->status_label), error.c_str());
      }
      return false;
    }
    if (value == row->field->default_value) {
      g_window->overrides.Clear(row->field->key);
    } else {
      g_window->overrides.Set(row->field->key, value);
    }
  }
  std::string error;
  if (!mocktail::runtime::SaveUiConfigOverrides(g_window->config_ui_file,
                                       g_window->overrides, &error)) {
    if (g_window->status_label != nullptr) {
      gtk_label_set_text(GTK_LABEL(g_window->status_label), error.c_str());
    }
    ShowToast("Could not save settings");
    return false;
  }
  RefreshAllRows();
  if (g_window->status_label != nullptr) {
    gtk_label_set_text(GTK_LABEL(g_window->status_label),
                       g_window->overrides.empty()
                           ? "Saved. No settings are set in this window."
                           : "Saved. Changes apply on the next launch.");
  }
  return true;
}

void OnSaveClicked(GtkWidget* button, gpointer user_data) {
  (void)button;
  (void)user_data;
  if (SaveChanges()) {
    ShowToast("Settings saved");
  }
}

void OnResetAllClicked(GtkWidget* button, gpointer user_data) {
  (void)button;
  (void)user_data;
  if (g_window == nullptr) {
    return;
  }
  AdwAlertDialog* dialog = ADW_ALERT_DIALOG(
      adw_alert_dialog_new("Reset everything?",
                           "Every setting this window manages goes back to "
                           "its default. Your config.yaml is not touched."));
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel",
                                  "Cancel");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "reset", "Reset");
  adw_alert_dialog_set_response_appearance(
      ADW_ALERT_DIALOG(dialog), "reset", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(+[](AdwAlertDialog* source,
                                                      char* response,
                                                      gpointer data) -> void {
                     (void)data;
                     if (std::string_view(response) != "reset") {
                       return;
                     }
                     if (g_window == nullptr) {
                       return;
                     }
                     g_window->overrides = UiConfigOverrides{};
                     for (FieldRow* row : g_window->rows) {
                       SetControlValue(row, row->field->default_value);
                     }
                     RefreshAllRows();
                     if (g_window->status_label != nullptr) {
                       gtk_label_set_text(
                           GTK_LABEL(g_window->status_label),
                           "Unsaved changes. They apply on the next launch.");
                     }
                     (void)source;
                   }),
                   nullptr);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(g_window->window));
}

// Relaunching drops whatever the user is in, so it is always asked for and
// never done without an answer. The environment is passed on so the relaunched
// process behaves identically.
// Closes this window shortly after a successful restart, so the new process
// can claim the display before this one lets go. A named function rather than a
// lambda inside G_CALLBACK, whose macro argument cannot contain a top-level
// comma.
gboolean CloseWindowAfterRestart(gpointer) {
  if (g_window != nullptr && g_window->window != nullptr) {
    gtk_window_close(GTK_WINDOW(g_window->window));
  }
  return G_SOURCE_REMOVE;
}

// Relaunches Nightcap, leaving the window open with the reason on screen if it
// cannot. Returns whether the app was started.
bool RestartNightcap(AdwDialog* source) {
  // The app, not this window: the two are installed into the same directory.
  //
  // Going through a shell was the original approach and it was wrong twice
  // over: the command line said /proc/self/exe, which inside a spawned shell is
  // the shell's own executable, so it started a shell instead of the app. fork
  // and exec avoids the shell entirely, and setsid detaches the new session so
  // closing this process does not take the app with it.
  const std::filesystem::path app =
      mocktail::runtime::SiblingExecutable("mocktail");
  if (app.empty()) {
    ShowToast("Cannot find Nightcap next to this window");
    adw_dialog_close(ADW_DIALOG(source));
    return false;
  }
  const pid_t child = fork();
  if (child < 0) {
    ShowToast(std::string("Could not restart: ") + std::strerror(errno));
    adw_dialog_close(ADW_DIALOG(source));
    return false;
  }
  if (child == 0) {
    setsid();
    // The app outlives this command, so it must not keep the caller's terminal
    // attached. A failure here is not worth reporting: the app has its own
    // logging and this window is about to close either way.
    const int null_device = ::open("/dev/null", O_RDWR);
    if (null_device >= 0) {
      ::dup2(null_device, STDIN_FILENO);
      ::dup2(null_device, STDOUT_FILENO);
      ::dup2(null_device, STDERR_FILENO);
      if (null_device > STDERR_FILENO) {
        ::close(null_device);
      }
    }
    const std::string path = app.string();
    const char* arguments[] = {path.c_str(), nullptr};
    execv(path.c_str(), const_cast<char* const*>(arguments));
    _exit(EXIT_FAILURE);
  }
  // The app is its own process now, so this window can go -- but only on this
  // path, so a failed spawn leaves the window up with the reason on screen
  // rather than closing and looking as though it worked.
  g_timeout_add(600, CloseWindowAfterRestart, nullptr);
  return true;
}

void OnRestartClicked(GtkWidget* button, gpointer user_data) {
  (void)button;
  (void)user_data;
  if (g_window == nullptr) {
    return;
  }
  AdwAlertDialog* dialog = ADW_ALERT_DIALOG(
      adw_alert_dialog_new("Restart Nightcap?",
                           "Settings are read at launch, so they take effect "
                           "after a restart. Anything you are in the middle "
                           "of will be closed."));
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel",
                                  "Cancel");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "restart",
                                  "Restart");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "restart",
                                             ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(+[](AdwAlertDialog* source,
                                                      char* response,
                                                      gpointer data) -> void {
                     (void)data;
                     if (std::string_view(response) != "restart" ||
                         g_window == nullptr) {
                       return;
                     }
                     // Save first: a restart that loses the change the user
                     // just made would be worse than not restarting.
                     if (!SaveChanges()) {
                       adw_dialog_close(ADW_DIALOG(source));
                       return;
                     }
                     RestartNightcap(ADW_DIALOG(source));
                   }),
                   nullptr);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(g_window->window));
}

// Returns gboolean because close-request does: returning anything non-zero
// stops the close. A void handler here left whatever was in the return
// register to decide, which blocked the window from closing at all.
gboolean OnCloseRequested(GtkWindow* window, gpointer user_data) {
  (void)window;
  (void)user_data;
  if (g_window != nullptr) {
    // Marked before anything else, because everything after this runs while GTK
    // is about to destroy the controls.
    g_window->tearing_down = true;
    // Every row's control and reset button, disconnected by the row pointer
    // they were connected with. GTK emits property notifications while tearing
    // widgets down, so a change signal can arrive after the widget it names is
    // already being freed; reading it then is a use-after-free. Disconnecting
    // stops the callback rather than relying on the flag alone to ignore it.
    for (FieldRow* row : g_window->rows) {
      if (row->control != nullptr) {
        g_signal_handlers_disconnect_by_data(row->control, row);
      }
      if (row->reset_button != nullptr) {
        g_signal_handlers_disconnect_by_data(row->reset_button, row);
      }
    }
    // Closing without saving must not leave a half-applied change in memory
    // that the next open would then show as saved. Anything the user did not
    // save is simply not written, which is what closing without saving means.
    g_window->overrides = UiConfigOverrides{};
    // The drop-down models are deliberately NOT released here. GTK is about to
    // destroy the drop-downs, and it asks its model for an item count while
    // doing so; freeing the model first leaves it reading a dangling pointer,
    // which is a list-model assertion and then an abort. The window is the
    // whole process and exits immediately, so the reference is left to go with
    // it rather than released at a moment when it cannot be released safely.
  }
  // FALSE allows the close to proceed.
  return FALSE;
}

GtkWidget* BuildFieldRow(FieldRow* row) {
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(list, 6);
  gtk_widget_set_margin_bottom(list, 6);

  GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
  GtkWidget* title = gtk_label_new(row->field->key.c_str());
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(title), TRUE);
  gtk_box_append(GTK_BOX(labels), title);
  GtkWidget* help = gtk_label_new(row->field->help.c_str());
  gtk_label_set_xalign(GTK_LABEL(help), 0.0f);
  gtk_widget_add_css_class(help, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(help), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(help), 52);
  gtk_box_append(GTK_BOX(labels), help);
  gtk_widget_set_hexpand(labels, TRUE);
  gtk_box_append(GTK_BOX(list), labels);

  if (row->field->boolean) {
    row->control = gtk_switch_new();
    gtk_widget_set_valign(row->control, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(row->control, GTK_ALIGN_END);
    g_signal_connect(row->control, "state-set",
                     G_CALLBACK(+[](GtkSwitch* source, gboolean state,
                                    gpointer data) -> gboolean {
                       (void)source;
                       (void)state;
                       OnFieldChanged(nullptr, data);
                       return TRUE;
                     }),
                     row);
  } else if (row->field->numeric) {
    row->control = gtk_spin_button_new_with_range(
        static_cast<double>(row->field->minimum),
        static_cast<double>(row->field->maximum), 1.0);
    gtk_widget_set_valign(row->control, GTK_ALIGN_CENTER);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(row->control), TRUE);
    g_signal_connect(row->control, "value-changed",
                     G_CALLBACK(+[](GtkSpinButton* source, gpointer data)
                                    -> void {
                       (void)source;
                       OnFieldChanged(nullptr, data);
                     }),
                     row);
  } else if (row->field->free_text) {
    row->control = gtk_entry_new();
    gtk_widget_set_valign(row->control, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(row->control, 260, -1);
    g_signal_connect(row->control, "changed",
                     G_CALLBACK(+[](GtkEditable* source, gpointer data)
                                    -> void {
                       (void)source;
                       OnFieldChanged(nullptr, data);
                     }),
                     row);
  } else {
    // gtk_string_list_new takes a NULL-terminated list of strings, not a
    // count, so the values are collected into a temporary vector of pointers.
    std::vector<const gchar*> strings;
    for (const std::string& value : row->field->allowed_values) {
      strings.push_back(value.c_str());
    }
    strings.push_back(nullptr);
    GtkStringList* options = gtk_string_list_new(strings.data());
    // Kept on the row: see option_model above.
    row->option_model = options;
    row->control = gtk_drop_down_new(G_LIST_MODEL(options), nullptr);
    gtk_widget_set_valign(row->control, GTK_ALIGN_CENTER);
    g_signal_connect(row->control, "notify::selected",
                     G_CALLBACK(+[](GObject* source, GParamSpec* spec,
                                    gpointer data) -> void {
                       (void)source;
                       (void)spec;
                       OnFieldChanged(nullptr, data);
                     }),
                     row);
  }
  gtk_box_append(GTK_BOX(list), row->control);

  row->reset_button = gtk_button_new_from_icon_name("edit-undo-symbolic");
  gtk_widget_set_valign(row->reset_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(row->reset_button,
                              "Clear this setting and use the default");
  g_signal_connect(row->reset_button, "clicked", G_CALLBACK(OnResetClicked),
                   row);
  gtk_box_append(GTK_BOX(list), row->reset_button);

  row->container = list;
  return list;
}

bool TabMatches(const std::string& tab, const UiConfigField& field) {
  if (tab == "Graphics") {
    return field.key.rfind("graphics.", 0) == 0 ||
           field.key.rfind("performance.", 0) == 0;
  }
  if (tab == "Input") {
    return field.key.rfind("input.", 0) == 0 ||
           field.key.rfind("compatibility.", 0) == 0;
  }
  if (tab == "Audio") {
    return field.key.rfind("audio.", 0) == 0;
  }
  if (tab == "Integrations") {
    return field.key.rfind("integrations.", 0) == 0;
  }
  return field.key.rfind("appearance.", 0) == 0 ||
         field.key.rfind("network.", 0) == 0 ||
         field.key.rfind("runtime.", 0) == 0;
}

void OnWindowActivate(AdwApplication* application, gpointer user_data) {
  (void)user_data;
  static bool created = false;
  if (created) {
    return;
  }
  created = true;

  auto* window = new Window();
  g_window = window;
  window->fields = mocktail::runtime::UiConfigFields();

  // The same paths the app resolves, so the window edits the file the app reads
  // rather than a path it guessed.
  const mocktail::runtime::RuntimePaths paths =
      mocktail::runtime::RuntimePaths::FromEnvironment(mocktail::runtime::ProcessEnvironment());
  window->config_ui_file =
      paths.config_file().parent_path() / mocktail::runtime::kUiConfigFileName;
  window->config_yaml_file = paths.config_file().string();

  bool ui_loaded = false;
  std::string error;
  if (!mocktail::runtime::LoadUiConfigOverrides(window->config_ui_file, &window->overrides,
                                      &ui_loaded, &error)) {
    // A file the window cannot read is reported rather than ignored: starting
    // from an empty set would let saving overwrite settings the user already
    // has.
    g_printerr("Cannot read %s: %s\n", window->config_ui_file.c_str(),
               error.c_str());
  }

  window->window =
      ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(
          application)));
  gtk_window_set_title(GTK_WINDOW(window->window), "Nightcap Settings");
  gtk_window_set_default_size(GTK_WINDOW(window->window), 760, 620);

  // A plain box rather than AdwToolbarView: the libadwaita available here does
  // not have it, and a window this simple does not need the extra machinery.
  GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_set_title_widget(
      ADW_HEADER_BAR(header),
      adw_window_title_new("Nightcap Settings",
                           "Changes apply on the next launch"));
  gtk_box_append(GTK_BOX(root), header);

  window->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());

  // The content goes inside the toast overlay rather than beside it, so a
  // toast floats over the window instead of the overlay claiming layout space
  // and squeezing the list.
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  adw_toast_overlay_set_child(window->toasts, box);
  gtk_widget_set_vexpand(GTK_WIDGET(window->toasts), TRUE);
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(window->toasts));

  // A banner explaining the two-file arrangement, because a user who
  // hand-edits config.yaml and then finds their change ignored needs to know
  // why without reading the source.
  GtkWidget* banner = adw_banner_new(
      "Settings saved here override your config.yaml. A value set in this "
      "window wins over the same key in config.yaml; clear it here to go back "
      "to your own value.");
  adw_banner_set_button_label(ADW_BANNER(banner), "Open config.yaml");
  adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
  g_signal_connect(banner, "button-clicked",
                   G_CALLBACK(+[](AdwBanner* source, gpointer data) -> void {
                     (void)source;
                     (void)data;
                     if (g_window == nullptr) {
                       return;
                     }
                     const std::string uri =
                         "file://" + g_window->config_yaml_file;
                     g_app_info_launch_default_for_uri(uri.c_str(), nullptr,
                                                       nullptr);
                   }),
                   nullptr);
  gtk_box_append(GTK_BOX(box), banner);

  GtkWidget* notebook = gtk_notebook_new();
  gtk_widget_set_vexpand(notebook, TRUE);
  gtk_box_append(GTK_BOX(box), notebook);

  for (const std::string& tab : mocktail::runtime::UiConfigTabs()) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 18);
    gtk_widget_set_margin_end(page, 18);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list),
                                    GTK_SELECTION_NONE);
    for (const UiConfigField& field : window->fields) {
      if (!TabMatches(tab, field)) {
        continue;
      }
      auto* row = new FieldRow();
      row->field = &field;
      const std::string* stored = window->overrides.Find(field.key);
      if (stored != nullptr) {
        row->original = *stored;
      }
      gtk_list_box_append(GTK_LIST_BOX(list), BuildFieldRow(row));
      window->rows.push_back(row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
    gtk_box_append(GTK_BOX(page), scroller);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page,
                             gtk_label_new(tab.c_str()));
  }

  GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(footer, 9);
  gtk_widget_set_margin_bottom(footer, 9);
  gtk_widget_set_margin_start(footer, 18);
  gtk_widget_set_margin_end(footer, 18);
  window->status_label = gtk_label_new(
      ui_loaded ? "Loaded from your config.ui.yaml. Edit and save to apply on "
                  "the next launch."
                : "Nothing set in this window yet. Changes are saved to "
                  "config.ui.yaml and apply on the next launch.");
  gtk_label_set_xalign(GTK_LABEL(window->status_label), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(window->status_label), TRUE);
  gtk_widget_set_hexpand(window->status_label, TRUE);
  gtk_box_append(GTK_BOX(footer), window->status_label);

  GtkWidget* reset_all = gtk_button_new_with_label("Reset all");
  gtk_widget_set_valign(reset_all, GTK_ALIGN_CENTER);
  g_signal_connect(reset_all, "clicked", G_CALLBACK(OnResetAllClicked),
                   nullptr);
  gtk_box_append(GTK_BOX(footer), reset_all);

  window->restart_button = gtk_button_new_with_label("Restart");
  gtk_widget_set_valign(window->restart_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(window->restart_button, "suggested-action");
  // Never restarts on its own. The user is asked first, and only after a
  // successful save, because a restart that dropped unsaved changes would be
  // worse than not restarting.
  gtk_widget_set_tooltip_text(window->restart_button,
                              "Apply the settings now. Asks first.");
  g_signal_connect(window->restart_button, "clicked",
                   G_CALLBACK(OnRestartClicked), nullptr);
  gtk_box_append(GTK_BOX(footer), window->restart_button);

  GtkWidget* save = gtk_button_new_with_label("Save");
  gtk_widget_set_valign(save, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(save, "suggested-action");
  g_signal_connect(save, "clicked", G_CALLBACK(OnSaveClicked), nullptr);
  gtk_box_append(GTK_BOX(footer), save);

  gtk_box_append(GTK_BOX(box), footer);
  adw_application_window_set_content(window->window, root);

  // Set the controls after they are all built, so setting one does not look
  // like a user edit and mark the window dirty on open.
  window->initializing = true;
  for (FieldRow* row : window->rows) {
    const std::string* stored = window->overrides.Find(row->field->key);
    const std::string value = stored != nullptr ? *stored
                                                 : row->field->default_value;
    SetControlValue(row, value);
    UpdateRowState(row);
  }
  window->initializing = false;
  RefreshAllRows();

  g_signal_connect(window->window, "close-request",
                   G_CALLBACK(OnCloseRequested), nullptr);
  gtk_window_present(GTK_WINDOW(window->window));
}

}  // namespace

int main(int argc, char* argv[]) {
  AdwApplication* application = adw_application_new(
      kApplicationId, G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "activate", G_CALLBACK(OnWindowActivate),
                   nullptr);
  const int status = g_application_run(G_APPLICATION(application), argc, argv);
  return status;
}
