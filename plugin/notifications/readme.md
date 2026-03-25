# Notifications Plugin

## Classes

### toast_notifier

#### Methods

##### set_app_id
Sets the Application User Model ID for notifications.

`void toast_notifier::set_app_id(const string &in id);`

###### Arguments:
* const string &in id: The AppUserModelID.

###### Remarks:
This ID must match the one used in `install_shortcut`. It groups notifications in the Action Center.

##### show
Shows a toast notification.

1. `bool toast_notifier::show(const string &in title, const string &in body);`
2. `bool toast_notifier::show(const string &in title, const string &in body, const string &in buttons, const string &in actions);`

###### Arguments (1):
* const string &in title: The notification title.
* const string &in body: The notification body text.

###### Arguments (2):
* const string &in title: The notification title.
* const string &in body: The notification body text.
* const string &in buttons: A pipe-separated list of button labels (e.g., "Yes|No").
* const string &in actions: A pipe-separated list of action IDs corresponding to the buttons (e.g., "yes_action|no_action").

###### Returns:
bool: true if the notification was shown.

###### Remarks:
When using buttons, the `actions` string defines the ID that will be returned by `get_action()` when a user clicks that button. The `actions` list must align with the `buttons` list.

###### Example:
```NVGT
toast_notifier notify;
notify.show("Update Available", "Download now?", "Yes|Later", "act_download|act_later");
```

##### install_shortcut
Installs a start menu shortcut required for toast notifications to work properly.

`bool toast_notifier::install_shortcut(const string &in app_id, const string &in app_name);`

###### Arguments:
* const string &in app_id: The AppUserModelID.
* const string &in app_name: The name of the shortcut to create in the Start Menu.

###### Returns:
bool: true if successful.

###### Remarks:
Windows Toast Notifications **require** a shortcut in the Start Menu with a valid AppUserModelID to function correctly. This method handles that setup.

##### get_action
Retrieves the action ID of the last clicked notification button.

`string toast_notifier::get_action();`

###### Returns:
string: The action ID, or empty string if none.

###### Example:
```NVGT
string action = notify.get_action();
if (action == "act_download") {
    // Start download...
}
```

---

### tray_icon

#### Methods

##### add
Adds an icon to the system tray.

`bool tray_icon::add(const string &in tooltip);`

###### Arguments:
* const string &in tooltip: The tooltip text for the tray icon.

###### Returns:
bool: true if successful.

##### remove
Removes the tray icon.

`void tray_icon::remove();`

##### set_tooltip
Updates the tray icon's tooltip.

`void tray_icon::set_tooltip(const string &in tooltip);`

###### Arguments:
* const string &in tooltip: The new tooltip text.

##### set_menu_items
Sets the items for the tray icon's context menu.

`void tray_icon::set_menu_items(const string &in items);`

###### Arguments:
* const string &in items: A pipe-separated list of menu items (e.g., "Open|Exit").

###### Remarks:
When a user right-clicks the tray icon, this menu will appear.

##### is_clicked
Checks if the tray icon was left-clicked.

`bool tray_icon::is_clicked();`

###### Returns:
bool: true if clicked since last check.

##### get_menu_click
Gets the index of the clicked context menu item.

`int tray_icon::get_menu_click();`

###### Returns:
int: The index of the clicked item (0-based), or -1 if none.

###### Example:
```NVGT
tray_icon tray;
tray.add("My Game");
tray.set_menu_items("Show|Exit");

// In loop
int click = tray.get_menu_click();
if (click == 0) {
    // Show window
} else if (click == 1) {
    exit();
}
```

##### hide_window
Hides a window.

`void tray_icon::hide_window(uint64 handle);`

###### Arguments:
* uint64 handle: The window handle.

##### show_window
Shows a window.

`void tray_icon::show_window(uint64 handle);`

###### Arguments:
* uint64 handle: The window handle.