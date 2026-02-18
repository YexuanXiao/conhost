diff --git a/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp b/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp
--- a/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp
+++ b/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp
@@ -3114,34 +3114,38 @@
                                                 // Perform the actual COM handoff. On success, COM returns
-                                                // a handle to the delegated host process so we can wait
-                                                // for it and keep PID continuity for clients that expect
-                                                // the original host process to remain alive.
-                                                auto delegated_process = invoke_console_handoff(
-                                                    delegation_target->value(),
-                                                    options.server_handle,
-                                                    input_available_event.view(),
-                                                    attach,
-                                                    signal_pipe_pair->write_end.view(),
-                                                    inbox_process->view(),
-                                                    logger);
-                                                if (delegated_process)
-                                                {
-                                                    const DWORD client_pid = attach.Process > std::numeric_limits<DWORD>::max()
-                                                        ? 0
-                                                        : static_cast<DWORD>(attach.Process);
-                                                    const DWORD delegated_pid = ::GetProcessId(delegated_process->get());
-                                                    if (delegated_pid != 0)
-                                                    {
-                                                        logger.log(
-                                                            logging::LogLevel::info,
-                                                            L"Default-terminal delegation established; delegated_host_pid={}, client_pid={}, waiting for delegated host exit",
-                                                            delegated_pid,
-                                                            client_pid);
-                                                    }
-                                                    else
-                                                    {
-                                                        logger.log(
-                                                            logging::LogLevel::info,
-                                                            L"Default-terminal delegation established; delegated_host_pid=<unavailable>, client_pid={}, waiting for delegated host exit",
-                                                            client_pid);
-                                                    }
+                                                // a handle to the delegated host process so we can wait
+                                                // for it and keep PID continuity for clients that expect
+                                                // the original host process to remain alive.
+                                                auto delegated_process = invoke_console_handoff(
+                                                    delegation_target->clsid,
+                                                    false,
+                                                    options.server_handle,
+                                                    input_available_event.view(),
+                                                    attach,
+                                                    signal_pipe_pair->write_end.view(),
+                                                    inbox_process->view(),
+                                                    logger);
+                                                if (delegated_process)
+                                                {
+                                                    if (delegated_process->has_value())
+                                                    {
+                                                        core::UniqueHandle delegated_process_handle = std::move(delegated_process->value());
+                                                        const DWORD client_pid = attach.Process > std::numeric_limits<DWORD>::max()
+                                                            ? 0
+                                                            : static_cast<DWORD>(attach.Process);
+                                                        const DWORD delegated_pid = ::GetProcessId(delegated_process_handle.get());
+                                                        if (delegated_pid != 0)
+                                                        {
+                                                            logger.log(
+                                                                logging::LogLevel::info,
+                                                                L"Default-terminal delegation established; delegated_host_pid={}, client_pid={}, waiting for delegated host exit",
+                                                                delegated_pid,
+                                                                client_pid);
+                                                        }
+                                                        else
+                                                        {
+                                                            logger.log(
+                                                                logging::LogLevel::info,
+                                                                L"Default-terminal delegation established; delegated_host_pid=<unavailable>, client_pid={}, waiting for delegated host exit",
+                                                                client_pid);
+                                                        }
 
