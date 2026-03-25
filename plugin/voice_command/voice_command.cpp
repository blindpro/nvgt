#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechRecognition.h>
#include "../../src/nvgt_plugin.h"

using namespace winrt;
using namespace Windows::Media::SpeechRecognition;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using std::string;
using std::vector;

constexpr size_t MAX_QUEUE_SIZE = 1000;
constexpr size_t MAX_COMMAND_LENGTH = 1024;
constexpr int MAX_RESTART_ATTEMPTS = 3;
constexpr int RESTART_DELAY_MS = 1000;

class CSpeechRecognizer {
public:
	CSpeechRecognizer() 
		: m_refCount(1)
		, m_active(false)
		, m_initialized(false)
		, m_stopping(false)
		, m_isDestroying(false)
		, m_errorCount(0)
		, m_restartAttempts(0) {
	}

	~CSpeechRecognizer() {
		m_isDestroying = true;
		stop();
		cleanup_internal();
	}

	void AddRef() {
		asAtomicInc(m_refCount);
	}

	void Release() {
		if (asAtomicDec(m_refCount) == 0) {
			delete this;
		}
	}

	bool setup() {
		std::lock_guard<std::mutex> lock(m_setupMutex);
		if (m_initialized) return true;
		try {
			try { init_apartment(); } catch (...) {}
			cleanup_internal();
			m_recognizer = SpeechRecognizer();
			m_resultToken = m_recognizer.ContinuousRecognitionSession().ResultGenerated({ this, &CSpeechRecognizer::OnResultGenerated });
			m_completedToken = m_recognizer.ContinuousRecognitionSession().Completed({ this, &CSpeechRecognizer::OnCompleted });
			m_hypothesisToken = m_recognizer.HypothesisGenerated({ this, &CSpeechRecognizer::OnHypothesisGenerated });
			m_stateChangedToken = m_recognizer.StateChanged({ this, &CSpeechRecognizer::OnStateChanged });
			m_initialized = true;
			m_errorCount = 0;
			m_restartAttempts = 0;
			push_debug("Setup complete (WinRT).");
			return true;
		} catch (hresult_error const& ex) {
			push_debug("Setup failed: " + to_string(ex.message()));
			m_initialized = false;
			return false;
		} catch (...) {
			push_debug("Setup failed: Unknown error.");
			m_initialized = false;
			return false;
		}
	}

	bool add_command(const string& command) {
		if (command.empty() || command.length() > MAX_COMMAND_LENGTH) return false;
		std::lock_guard<std::mutex> lock(m_dataMutex);
		hstring cmd_hstring = to_hstring(command);
		for(const auto& existing : m_commands) {
			if(existing == cmd_hstring) return false;
		}
		m_commands.push_back(cmd_hstring);
		return true;
	}

	void clear_commands() {
		std::lock_guard<std::mutex> lock(m_dataMutex);
		m_commands.clear();
	}

	void start() {
		if (!m_initialized || m_isDestroying) {
			push_debug("Cannot start: Not initialized or destroying.");
			return;
		}
		bool expected = false;
		if (m_active) return;
		m_stopping = false;
		try {
			std::vector<hstring> fixed_commands;
			bool enable_dictation = false;
			{
				std::lock_guard<std::mutex> lock(m_dataMutex);
				for (const auto& cmd : m_commands) {
					if (cmd == L"type *" || cmd == L"*") {
						enable_dictation = true;
					} else {
						fixed_commands.push_back(cmd);
					}
				}
			}
			m_recognizer.Constraints().Clear();
			if (!fixed_commands.empty()) {
				SpeechRecognitionListConstraint listConstraint(fixed_commands);
				m_recognizer.Constraints().Append(listConstraint);
			}
			if (enable_dictation) {
				SpeechRecognitionTopicConstraint dictationConstraint(SpeechRecognitionScenario::Dictation, L"Dictation");
				m_recognizer.Constraints().Append(dictationConstraint);
			}
			this->AddRef();
			auto compileOp = m_recognizer.CompileConstraintsAsync();
			compileOp.Completed([this](IAsyncOperation<SpeechRecognitionCompilationResult> const& info, AsyncStatus status) {
				if (m_isDestroying) {
					this->Release();
					return;
				}
				if (status != AsyncStatus::Completed) {
					push_debug("Compilation failed/cancelled.");
					handle_error();
					this->Release();
					return;
				}
				try {
					SpeechRecognitionCompilationResult result = info.GetResults();
					if (result.Status() != SpeechRecognitionResultStatus::Success) {
						push_debug("Compilation status failure.");
						handle_error();
						this->Release();
						return;
					}
					auto startOp = m_recognizer.ContinuousRecognitionSession().StartAsync();
					startOp.Completed([this](IAsyncAction const&, AsyncStatus status) {
						if (!m_isDestroying) {
							if (status == AsyncStatus::Completed) {
								m_active = true;
								m_errorCount = 0;
								m_restartAttempts = 0;
								push_debug("Listening.");
							} else {
								push_debug("StartAsync failed.");
								handle_error();
							}
						}
						this->Release(); 
					});
				} catch (...) {
					push_debug("Async Compilation Exception.");
					handle_error();
					this->Release();
				}
			});
		} catch (...) {
			push_debug("Start init failed.");
			handle_error();
		}
	}

	void stop() {
		m_stopping = true;
		if (!m_recognizer) return;
		try {
			m_recognizer.ContinuousRecognitionSession().StopAsync();
			m_active = false;
		} catch (...) {}
	}

	bool get_active() const { return m_active; }
	bool get_initialized() const { return m_initialized; }

	uint32_t get_commands_pending() {
		std::lock_guard<std::mutex> lock(m_queueMutex);
		return (uint32_t)m_commandQueue.size();
	}

	string get_command() {
		std::lock_guard<std::mutex> lock(m_queueMutex);
		if (m_commandQueue.empty()) return "";
		string cmd = m_commandQueue.front();
		m_commandQueue.pop();
		return cmd;
	}

	void clear_queue() {
		std::lock_guard<std::mutex> lock(m_queueMutex);
		std::queue<string> empty;
		std::swap(m_commandQueue, empty);
	}

	uint32_t get_error_count() const { return m_errorCount; }
	void reset_error_count() { m_errorCount = 0; m_restartAttempts = 0; }

private:
	int m_refCount;
	std::atomic<bool> m_active;
	std::atomic<bool> m_initialized;
	std::atomic<bool> m_stopping;
	std::atomic<bool> m_isDestroying;
	std::atomic<uint32_t> m_errorCount;
	std::atomic<int> m_restartAttempts;
	SpeechRecognizer m_recognizer{ nullptr };
	event_token m_resultToken{};
	event_token m_completedToken{};
	event_token m_hypothesisToken{};
	event_token m_stateChangedToken{};
	std::vector<hstring> m_commands;
	std::mutex m_dataMutex;
	std::mutex m_setupMutex;
	std::queue<string> m_commandQueue;
	std::mutex m_queueMutex;

	void cleanup_internal() {
		if (m_recognizer) {
			try {
				if (m_resultToken.value) m_recognizer.ContinuousRecognitionSession().ResultGenerated(m_resultToken);
				if (m_completedToken.value) m_recognizer.ContinuousRecognitionSession().Completed(m_completedToken);
				if (m_hypothesisToken.value) m_recognizer.HypothesisGenerated(m_hypothesisToken);
				if (m_stateChangedToken.value) m_recognizer.StateChanged(m_stateChangedToken);
			} catch (...) {}
			m_recognizer = nullptr;
		}
	}

	void handle_error() {
		if (m_stopping || m_isDestroying) return;
		m_errorCount++;
		if (m_errorCount < MAX_RESTART_ATTEMPTS && m_initialized) {
			m_restartAttempts++;
			this->AddRef(); 
			std::thread([this]() {
				std::this_thread::sleep_for(std::chrono::milliseconds(RESTART_DELAY_MS));
				if (!m_isDestroying && !m_stopping) {
					start(); 
				}
				this->Release();
			}).detach();
		}
	}

	void OnResultGenerated(SpeechContinuousRecognitionSession const&, SpeechContinuousRecognitionResultGeneratedEventArgs const& args) {
		try {
			if (args.Result().Status() == SpeechRecognitionResultStatus::Success) {
				string text = to_string(args.Result().Text());
				if (!text.empty()) push_message(text);
			}
		} catch (...) {}
	}

	void OnHypothesisGenerated(SpeechRecognizer const&, SpeechRecognitionHypothesisGeneratedEventArgs const& args) {
	}

	void OnStateChanged(SpeechRecognizer const&, SpeechRecognizerStateChangedEventArgs const& args) {
	}

	void OnCompleted(SpeechContinuousRecognitionSession const&, SpeechContinuousRecognitionCompletedEventArgs const& args) {
		m_active = false;
		if (!m_stopping && !m_isDestroying) {
			SpeechRecognitionResultStatus status = args.Status();
			if (status != SpeechRecognitionResultStatus::Success && status != SpeechRecognitionResultStatus::UserCanceled) {
				handle_error();
			}
		}
	}

	void push_message(const string& msg) {
		std::lock_guard<std::mutex> lock(m_queueMutex);
		if (m_commandQueue.size() >= MAX_QUEUE_SIZE) m_commandQueue.pop();
		m_commandQueue.push(msg);
	}

	void push_debug(const string& msg) {
	}
};

CSpeechRecognizer* RecoFactory() {
	return new CSpeechRecognizer();
}

plugin_main(nvgt_plugin_shared* shared) {
	asIScriptEngine* script_engine = shared->script_engine;
	if (!prepare_plugin(shared)) return false;
	int r = 0;
	r = script_engine->RegisterObjectType("voice_recognizer", sizeof(CSpeechRecognizer), asOBJ_REF);
	r = script_engine->RegisterObjectBehaviour("voice_recognizer", asBEHAVE_FACTORY, "voice_recognizer@ f()", asFUNCTION(RecoFactory), asCALL_CDECL);
	r = script_engine->RegisterObjectBehaviour("voice_recognizer", asBEHAVE_ADDREF, "void f()", asMETHOD(CSpeechRecognizer, AddRef), asCALL_THISCALL);
	r = script_engine->RegisterObjectBehaviour("voice_recognizer", asBEHAVE_RELEASE, "void f()", asMETHOD(CSpeechRecognizer, Release), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "bool setup()", asMETHOD(CSpeechRecognizer, setup), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "bool add_command(const string &in command)", asMETHOD(CSpeechRecognizer, add_command), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "void clear_commands()", asMETHOD(CSpeechRecognizer, clear_commands), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "void start()", asMETHOD(CSpeechRecognizer, start), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "void stop()", asMETHOD(CSpeechRecognizer, stop), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "bool get_active() const property", asMETHOD(CSpeechRecognizer, get_active), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "bool get_initialized() const property", asMETHOD(CSpeechRecognizer, get_initialized), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "uint get_commands_pending() const property", asMETHOD(CSpeechRecognizer, get_commands_pending), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "string get_command()", asMETHOD(CSpeechRecognizer, get_command), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "void clear_queue()", asMETHOD(CSpeechRecognizer, clear_queue), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "uint get_error_count() const property", asMETHOD(CSpeechRecognizer, get_error_count), asCALL_THISCALL);
	r = script_engine->RegisterObjectMethod("voice_recognizer", "void reset_error_count()", asMETHOD(CSpeechRecognizer, reset_error_count), asCALL_THISCALL);
	return true;
}
