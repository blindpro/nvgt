# Voice Command Plugin

## Classes

### voice_recognizer

#### Methods

##### setup
Initializes the voice recognition system.

`bool voice_recognizer::setup();`

###### Returns:
bool: true if initialization was successful, false otherwise.

###### Remarks:
This uses the Windows Runtime (WinRT) Speech Recognition API. Ensure the system has a microphone configured and speech recognition enabled.

##### add_command
Adds a command to the recognition list.

`bool voice_recognizer::add_command(const string &in command);`

###### Arguments:
* const string &in command: The command phrase to listen for.

###### Returns:
bool: true if the command was added, false if it already exists or is invalid.

###### Remarks:
You can use standard phrases or special wildcards.
*   Specific phrase: "open inventory"
*   Dictation: "type *" or "*" will attempt to capture free-form speech.

###### Example:
```NVGT
voice_recognizer vr;
vr.add_command("fire");
vr.add_command("jump");
vr.add_command("reload");
```

##### clear_commands
Clears all registered commands.

`void voice_recognizer::clear_commands();`

##### start
Starts listening for commands.

`void voice_recognizer::start();`

###### Remarks:
This is an asynchronous operation. The recognizer will run in the background.

##### stop
Stops listening.

`void voice_recognizer::stop();`

##### get_active
Checks if the recognizer is currently active and listening.

`bool voice_recognizer::get_active() const property;`

###### Returns:
bool: true if active.

##### get_initialized
Checks if the recognizer is initialized.

`bool voice_recognizer::get_initialized() const property;`

###### Returns:
bool: true if initialized.

##### get_commands_pending
Gets the number of recognized commands waiting in the queue.

`uint voice_recognizer::get_commands_pending() const property;`

###### Returns:
uint: Number of pending commands.

##### get_command
Retrieves the next recognized command from the queue.

`string voice_recognizer::get_command();`

###### Returns:
string: The recognized command text, or empty string if queue is empty.

###### Example:
```NVGT
// Inside game loop
while (vr.get_commands_pending() > 0) {
    string cmd = vr.get_command();
    if (cmd == "fire") {
        player.shoot();
    } else if (cmd == "jump") {
        player.jump();
    }
}
```

##### clear_queue
Clears the queue of pending recognized commands.

`void voice_recognizer::clear_queue();`

##### get_error_count
Gets the number of errors encountered.

`uint voice_recognizer::get_error_count() const property;`

###### Returns:
uint: Error count.

##### reset_error_count
Resets the error counter.

`void voice_recognizer::reset_error_count();`