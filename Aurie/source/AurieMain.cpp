// Include NTSTATUS values from this header, because it has all of them
#include <ntstatus.h>

// Don't include NTSTATUS values from Windows.h and winternl.h
#define WIN32_NO_STATUS
#include <Windows.h>
#include <winternl.h>

#include "framework/framework.hpp"


// Unload routine, frees everything properly
void ArProcessDetach(HINSTANCE)
{
	using namespace Aurie;

	// Unload all modules except the initial image
	// First calls the ModuleUnload functions (if they're set up)
	for (auto& entry : Internal::g_LdrModuleList)
	{
		// Skip the initial image
		if (&entry == g_ArInitialImage)
			continue;
		
		// Unmap the image (but don't remove it from the list)
		Internal::MdpUnmapImage(
			&entry,
			false,
			true
		);
	}

	// Free persistent memory
	for (auto& allocation : g_ArInitialImage->MemoryAllocations)
	{
		Internal::MmpFreeMemory(
			g_ArInitialImage,
			allocation.AllocationBase,
			false
		);
	}

	// Remove all the allocations, they're now invalid
	g_ArInitialImage->MemoryAllocations.clear();

	// Null the initial image, and clear the module list
	g_ArInitialImage = nullptr;
	Internal::g_LdrModuleList.clear();
}

// Called upon framework initialization (DLL_PROCESS_ATTACH) event.
// This is the first function that runs.
void ArProcessAttach(HINSTANCE Instance)
{
	using namespace Aurie;

	// Query the image path
	DWORD process_name_size = MAX_PATH;
	wchar_t process_name[MAX_PATH] = { 0 };
	if (!QueryFullProcessImageNameW(
		GetCurrentProcess(),
		0,
		process_name,
		&process_name_size
	))
	{
		return (void)MessageBoxA(
			nullptr,
			"Failed to query process path!",
			"Aurie Framework",
			MB_OK | MB_TOPMOST | MB_ICONERROR | MB_SETFOREGROUND
		);
	}

	// Create the initial image for the Aurie Framework.
	AurieModule initial_module;
	if (!AurieSuccess(
		Internal::MdpCreateModule(
			process_name,
			Instance,
			false,
			0,
			initial_module
		)
	))
	{
		return (void)MessageBoxA(
			nullptr,
			"Failed to create initial module!",
			"Aurie Framework",
			MB_OK | MB_TOPMOST | MB_ICONERROR | MB_SETFOREGROUND
		);
	}

	g_ArInitialImage = Internal::MdpAddModuleToList(
		std::move(initial_module)
	);

	// Get the current folder (where the main executable is)
	fs::path folder_path;
	if (!AurieSuccess(
		Internal::MdpGetImageFolder(
			g_ArInitialImage, 
			folder_path
		)
	))
	{
		return (void)MessageBoxA(
			nullptr,
			"Failed to get initial folder!",
			"Aurie Framework",
			MB_OK | MB_TOPMOST | MB_ICONERROR | MB_SETFOREGROUND
		);
	}

	// Craft the path from which the mods will be loaded
	folder_path = folder_path / "mods" / "aurie";

	// Load everything from %APPDIR%\\mods\\aurie
	Internal::MdpMapFolder(
		folder_path,
		true,
		false,
		nullptr
	);

	// Call ModulePreload on all loaded plugins
	for (auto& entry : Internal::g_LdrModuleList)
	{
		AurieStatus last_status = Internal::MdpDispatchEntry(
			&entry,
			entry.ModulePreinitialize
		);

		// Mark mods failed for loading for the purge
		if (!AurieSuccess(last_status))
			Internal::MdpMarkModuleForPurge(&entry);
		else
			entry.Flags.IsPreloaded = true;
	}

	// Purge all the modules that failed loading
	// We can't do this in the for loop because of iterators...
	Internal::MdpPurgeMarkedModules();

	// Resume our process if needed
	bool is_process_suspended = false;
	if (!AurieSuccess(ElIsProcessSuspended(is_process_suspended)) || is_process_suspended)
	{
		Internal::ElpResumeProcess(GetCurrentProcess());
	}

	// Now we have to wait until the current process has finished initializating
	
	// Query the process subsystem
	unsigned short current_process_subsystem = 0;
	PpGetImageSubsystem(
		GetModuleHandleA(nullptr),
		current_process_subsystem
	);

	// If the current process is a GUI process, wait for its window
	if (current_process_subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI)
		ElWaitForCurrentProcessWindow();

	WaitForInputIdle(GetCurrentProcess(), INFINITE);

	// Call ModuleEntry on all loaded plugins
	for (auto& entry : Internal::g_LdrModuleList)
	{
		// Ignore modules that are already initialized?
		if (MdIsImageInitialized(&entry))
			continue;

		AurieStatus last_status = Internal::MdpDispatchEntry(
			&entry,
			entry.ModuleInitialize
		);

		// Mark mods failed for loading for the purge
		if (!AurieSuccess(last_status))
			Internal::MdpMarkModuleForPurge(&entry);
		else
			entry.Flags.IsInitialized = true;
	}

	// Purge all the modules that failed loading
	// We can't do this in the for loop because of iterators...
	Internal::MdpPurgeMarkedModules();

	while (!GetAsyncKeyState(VK_END))
	{
		Sleep(1);
	}

	// Calls DllMain with DLL_PROCESS_DETACH, which calls ArProcessDetach
	FreeLibraryAndExitThread(Instance, 0);
}

BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,  // handle to DLL module
	DWORD fdwReason,     // reason for calling function
	LPVOID lpvReserved   // reserved
)  
{
	// Perform actions based on the reason for calling.
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			DisableThreadLibraryCalls(hinstDLL);

			HANDLE created_thread = CreateThread(
				nullptr,
				0,
				reinterpret_cast<LPTHREAD_START_ROUTINE>(ArProcessAttach),
				hinstDLL,
				0,
				nullptr
			);

			if (!created_thread)
				return FALSE;

			CloseHandle(created_thread);
			break;
		}
	case DLL_PROCESS_DETACH:
		{
			// Process termination, the kernel will free stuff for us.
			if (lpvReserved)
				return TRUE;

			ArProcessDetach(hinstDLL);

			break;
		}
	}

	return TRUE;
}