set(source_file "${CASC_SOURCE_ROOT}/common/FileTree.cpp")
file(READ "${source_file}" source_text)

if(source_text MATCHES "Prefer a locally available variant, and high resolution")
  return()
endif()

set(anchor [=[
    }
    return pFileNode;
}

PCASC_FILE_NODE CASC_FILE_TREE::ItemAt]=])
set(replacement [=[
    }
    else
    {
        DWORD ExistingContentFlags = 0;
        GetExtras(pFileNode, NULL, NULL, &ExistingContentFlags);

        const bool ExistingLocal = (pFileNode->pCKeyEntry->Flags & CASC_CE_FILE_IS_LOCAL) != 0;
        const bool NewLocal = (pCKeyEntry->Flags & CASC_CE_FILE_IS_LOCAL) != 0;
        const bool ExistingHighRes = (ExistingContentFlags & 0x1) != 0;
        const bool NewHighRes = (ContentFlags & 0x1) != 0;

        // Prefer a locally available variant, and high resolution when both are local.
        if((NewLocal && !ExistingLocal) || (NewLocal == ExistingLocal && NewHighRes && !ExistingHighRes))
        {
            pFileNode->pCKeyEntry->RefCount--;
            pFileNode->pCKeyEntry = pCKeyEntry;
            pCKeyEntry->RefCount++;
            SetExtras(pFileNode, FileDataId, LocaleFlags, ContentFlags);
        }
    }
    return pFileNode;
}

PCASC_FILE_NODE CASC_FILE_TREE::ItemAt]=])

string(FIND "${source_text}" "${anchor}" anchor_index)
if(anchor_index EQUAL -1)
  message(FATAL_ERROR "Unsupported CascLib FileTree.cpp layout")
endif()

string(REPLACE "${anchor}" "${replacement}" source_text "${source_text}")
file(WRITE "${source_file}" "${source_text}")
