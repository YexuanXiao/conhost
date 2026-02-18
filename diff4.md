diff --git a/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp b/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp
--- a/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp
+++ b/c:\Users\xyx\Documents\OpenConsole\new/src/runtime/session.cpp
@@ -3362,6 +3362,6 @@
                                                         // - 2: host-signal input thread (pipe disconnect indicator)
-                                                        if (observe_delegated)
-                                                        {
-                                                            add_handle(delegated_process->get(), 0);
-                                                        }
+                                                        if (observe_delegated)
+                                                        {
+                                                            add_handle(delegated_process_handle.get(), 0);
+                                                        }
                                                         if (console_reference)
@@ -3403,8 +3403,8 @@
 
-                                                            if (::GetExitCodeProcess(delegated_process->get(), &exit_code) == FALSE)
-                                                            {
-                                                                const DWORD error = ::GetLastError();
-                                                                if (error == ERROR_ACCESS_DENIED)
-                                                                {
-                                                                    logger.log(
+                                                            if (::GetExitCodeProcess(delegated_process_handle.get(), &exit_code) == FALSE)
+                                                            {
+                                                                const DWORD error = ::GetLastError();
+                                                                if (error == ERROR_ACCESS_DENIED)
+                                                                {
+                                                                    logger.log(
                                                                         logging::LogLevel::debug,
