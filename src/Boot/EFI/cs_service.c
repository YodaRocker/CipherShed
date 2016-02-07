/*  cs_service.c - CipherShed EFI boot loader
 *  implementation of the service menu of the user interface
 *
 *	Copyright (c) 2015-2016  Falk Nedwal
 *
 *	Governed by the Apache 2.0 License the full text of which is contained in
 *	the file License.txt included in CipherShed binary and source code distribution
 *	packages.
 */

#include <efi.h>
#include <efilib.h>

#include "cs_service.h"
#include "cs_controller.h"
#include <edk2/ComponentName.h>

#define CS_SERVICE_NUMBER_SECTORS	80	/* number sectors to encrypt/decrypt at once while encryption/decryption
 	 	 	 	 	 	 	 	 	 	   of the media using the service menu */

EFI_GUID ComponentNameProtocol = EFI_COMPONENT_NAME_PROTOCOL_GUID;

/*
 *	\brief	request the user password
 *
 *	This function asks the user to input the volume password. He can olso press ESC to
 *	interrupt the process. This behavior is reflected in the "user_decision".
 *
 *	\param	SystemTable		system table
 *	\param	options			options as defined by the user (taken from options file)
 *	\param	user_decision	buffer to define the next activity after the function returns
 *	\param	passwd			buffer for the user password
 *
 *	\return		the success state of the function
 */
static EFI_STATUS check_user_password(IN EFI_SYSTEM_TABLE *SystemTable,	IN const struct cs_option_data *options,
		OUT enum cs_enum_user_decision *user_decision, OUT Password *passwd) {
    EFI_STATUS error;
    EFI_INPUT_KEY key;
    BOOLEAN showWrongPwd = FALSE;

    ASSERT(SystemTable != NULL);
    ASSERT(options != NULL);
    ASSERT(user_decision != NULL);
    ASSERT(passwd != NULL);

    if (!options->flags.silent) {
        error = uefi_call_wrapper(SystemTable->ConOut->OutputString, 2, SystemTable->ConOut, L"\r\n\n ");
    	if (EFI_ERROR(error)) return error;
    }

    do {
        error = ask_for_pwd(SystemTable->ConOut, showWrongPwd, CS_STR_ENTER_PASSWD);
    	if (EFI_ERROR(error)) {
    		CS_DEBUG((D_ERROR, L"unable to output string (%r)\n", error));
    		return error;
    	}

    	error = get_input(SystemTable->ConIn, options->flags.enable_password_asterisk ? SystemTable->ConOut : NULL,
    			&passwd->Text, sizeof(passwd->Text), FALSE /* F8 */, FALSE /* dump */, TRUE /* ASCII */, &key);
    	if (EFI_ERROR(error)) {
    		CS_DEBUG((D_ERROR, L"unable to read input string (%r)\n", error));
    		return error;
    	}

        if (key.ScanCode == 0x17) { /* ESCAPE */
        	*user_decision = CS_UI_ESC_PRESSED;
        	return EFI_SUCCESS;
        }
    	passwd->Length = strlena(passwd->Text);
    	showWrongPwd = TRUE;

    	error = decrypt_volume_header();

    } while (error == EFI_ACCESS_DENIED);

    return error;
}

/*
 *	\brief	get a child handle of the given (media) handle that represents the CipherShed controller
 *
 *	This function searches for all handles that support BlockIo protocol and check which one has opened
 *	the BlockIo protocol of the given parent. With this list, the function checks the parent-child
 *	relationship and checks all children: To make sure that it is really the CipherShed device
 *	as generated by the CipherShed driver, the existence of the CsCallerIdGuid protocol is checked.
 *
 *	\param	ParentHandle	handle of the parent device (the encrypted media)
 *	\param	ChildHandle		pointer to buffer for child handle representing the crypto device
 *							handled by the CipherShed crypto driver
 *
 *	\return		the success state of the function
 */
static EFI_STATUS get_cs_child(IN EFI_HANDLE ParentHandle, OUT EFI_HANDLE *ChildHandle) {
	EFI_STATUS error;
	EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *OpenInfo = NULL;
	UINTN OpenInfoCount, OpenInfoIndex;

    ASSERT(ParentHandle != NULL);
    ASSERT(ChildHandle != NULL);

	*ChildHandle = NULL;

	/* Retrieve the list of agents that have opened each protocol */
	error = uefi_call_wrapper(BS->OpenProtocolInformation, 4,
			ParentHandle, &BlockIoProtocol, &OpenInfo, &OpenInfoCount);
	if (EFI_ERROR(error)) {
	    CS_DEBUG((D_ERROR, L"unable to get open protocol information (handle=0x%x): %r\n",
	    		ParentHandle, error));
		return error;
	}
    for (OpenInfoIndex = 0; OpenInfoIndex < OpenInfoCount; OpenInfoIndex++) {
        if ((OpenInfo[OpenInfoIndex].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)
        		== EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
        	EFI_HANDLE child;

        	child = OpenInfo[OpenInfoIndex].ControllerHandle;
        	CS_DEBUG((D_INFO, L"child handle for parent 0x%x found: 0x%x\n", ParentHandle, child));

        	/* now double check if the child is really the CipherShed device... */
        	if (is_cs_child_device(ParentHandle, child)) {
				*ChildHandle = child;
    			CS_DEBUG((D_INFO, L"valid child device found...\n"));
    			break;
    		} else {
    			CS_DEBUG((D_INFO, L"no valid child device!\n"));
    		}
        }
    }

    if (OpenInfo) {
    	FreePool(OpenInfo);
    }
    if (*ChildHandle == NULL) {
    	CS_DEBUG((D_ERROR, L"no matching child device found\n"));
    	error = EFI_NO_MEDIA;
    }

	return error;
}

/*
 *	\brief	open the Block I/O protocol of the given handle
 *
 *
 *	\param	handle	controller handle whose Block IO protocol shall be opened
 *	\param	blockIo	buffer for the returned protocol interface
 *
 *	\return		the success state of the function
 */
static EFI_STATUS open_blockio_protocol(IN EFI_HANDLE handle, OUT EFI_BLOCK_IO **blockIo) {
    EFI_STATUS error;

	ASSERT(blockIo != NULL);

	/* open the protocols that are consumed by the driver: BlockIoProtocol and BlockIo2Protocol */
	error = uefi_call_wrapper(BS->OpenProtocol, 6, handle, &BlockIoProtocol,
			(VOID **) blockIo, handle, handle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

	return error;
}

/*
 *	\brief	close the Block I/O protocol of the given handle
 *
 *
 *	\param	handle		controller handle whose Block IO protocol shall be closed
 *	\param	blockIo		the opened Block IO protocol interface
 *
 *	\return		the success state of the function
 */
static EFI_STATUS close_blockio_protocol(IN EFI_HANDLE handle, IN EFI_BLOCK_IO *blockIo) {
    EFI_STATUS error;

	ASSERT(blockIo != NULL);

	/* open the protocols that are consumed by the driver: BlockIoProtocol and BlockIo2Protocol */
	error = uefi_call_wrapper(BS->CloseProtocol, 4, handle, &BlockIoProtocol, handle, handle);

	return error;
}

/*
 *	\brief	open the Block I/O protocol of the two given handles
 *
 *  Tries to open the Block IO protocol of the given two handles. In case that at least one
 *  protocol interface cannot be opened, the function fails and both protocol interfaces are closed
 *
 *	\param	parentHandle	first controller handle whose Block IO protocol shall be opened
 *	\param	childHandle		second controller handle whose Block IO protocol shall be opened
 *	\param	parentBlockIo	buffer for the returned protocol interface for first handle
 *	\param	childBlockIo	buffer for the returned protocol interface for second handle
 *
 *	\return		the success state of the function
 */
static EFI_STATUS open_blockio_protocols(IN EFI_HANDLE parentHandle, IN EFI_HANDLE childHandle,
		OUT EFI_BLOCK_IO **parentBlockIo, OUT EFI_BLOCK_IO **childBlockIo) {
    EFI_STATUS error;


	ASSERT(parentBlockIo != NULL);
	ASSERT(childBlockIo != NULL);

	error = open_blockio_protocol(parentHandle, parentBlockIo);
	if (!EFI_ERROR(error)) {
		error = open_blockio_protocol(childHandle, childBlockIo);
		if (EFI_ERROR(error)) {
		    EFI_STATUS error2;

	    	CS_DEBUG((D_ERROR, L"unable to open child BlockIO protocol: %r\n", error));
	    	error2 = close_blockio_protocol(parentHandle, *parentBlockIo);
			if (EFI_ERROR(error2)) {
		    	CS_DEBUG((D_ERROR, L"unable to close parent BlockIO protocol: %r\n", error2));
			}
		}
	} else {
    	CS_DEBUG((D_ERROR, L"unable to open parent BlockIO protocol: %r\n", error));
	}

	return error;
}

/*
 *	\brief	update the encrypted blocks in CRYPTO_INFO and write it back to the volume header
 *
 *	The function modifies the crypto_info->EncryptedAreaLength value in the volume header end
 *	writes the volume header back
 *
 *	\param	processedLba	the LBA where the last crypto operation was successfully processed
 *	\param	crypto_info		pointer to the CRYPTO_INFO as contained in the volume header
 *	\param	encrypt		flag whether the device need to be encrypted or decrypted
 *
 *	\return		the success state of the function
 */
static EFI_STATUS update_blocks_in_volume_header(IN UINT64 processedLba, IN CRYPTO_INFO *crypto_info,
		IN BOOLEAN encrypt) {
    EFI_STATUS error;
    UINT64 startSector;
    UINTN numberSectors, sectorsInVolume;

    ASSERT(crypto_info != NULL);

    startSector = crypto_info->EncryptedAreaStart.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR;
    sectorsInVolume = crypto_info->VolumeSize.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR;

    if ((processedLba < startSector) || (processedLba > (startSector + sectorsInVolume))) {
    	CS_DEBUG((D_ERROR, L"inconsistent volume information: startSector: 0x%x, current LBA: 0x%x, vol size: 0x%x\n",
    			startSector, processedLba, sectorsInVolume));
    	return EFI_VOLUME_CORRUPTED;
    }
    numberSectors = processedLba - startSector;
    crypto_info->EncryptedAreaLength.Value = numberSectors << TC_LB_SIZE_BIT_SHIFT_DIVISOR;

    CS_DEBUG((D_INFO, L"update EncryptedAreaLength to value 0x%x (sector 0x%x)\n",
    		crypto_info->EncryptedAreaLength.Value, numberSectors));

    /* update volume header with crypto_info */
    error = update_volume_header(crypto_info);

    return error;
}

/*
 *	\brief	performs the media encryption/decryption triggered by the service menu
 *
 *  Based on the given flag "encrypt", the function encrypts or decrypts the blocks of the
 *  media based in the given block IO interface handles. The blocks to be encrypted/decrypted
 *  dependent on the information taken from the volume header.
 *  The function loops over the remaining blocks and reads from one device and writes to the
 *  other device until all blocks (taken from volume header) are processed or the user pressed
 *  the ESC key.
 *
 *	\param	SystemTable		system table
 *	\param	parentBlockIo	block IO interface of the parent device (without the CS crypto driver)
 *	\param	childBlockIo	block IO interface of the child device using the CS crypto driver
 *	\param	encrypt		flag whether the device need to be encrypted or decrypted
 *
 *	\return		the success state of the function
 */
static EFI_STATUS do_encrypt_decrypt_media(IN EFI_SYSTEM_TABLE *SystemTable,
		IN EFI_BLOCK_IO *parentBlockIo, IN EFI_BLOCK_IO *childBlockIo, IN BOOLEAN encrypt) {
    EFI_STATUS error;
    EFI_BLOCK_IO *source, *dest;
    PCRYPTO_INFO cryptoInfo;
    UINT64 startSector, endEncryptedArea, lba;
    UINTN numberSectors, numberEncryptedSectors, sectorsInVolume;
    void *buffer;
    const UINTN bufferSize = CS_SERVICE_NUMBER_SECTORS << TC_LB_SIZE_BIT_SHIFT_DIVISOR;
	UINTN bufferSectors = CS_SERVICE_NUMBER_SECTORS;

    ASSERT(parentBlockIo != NULL);
    ASSERT(childBlockIo != NULL);

    cryptoInfo = get_crypto_info();
    ASSERT(cryptoInfo != NULL);

    //startSector = cryptoInfo->EncryptedAreaStart.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR;
    startSector = 0; /* relative to the partition/media, not to the entire disk device */
    numberEncryptedSectors = cryptoInfo->EncryptedAreaLength.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR;
    endEncryptedArea = startSector + numberEncryptedSectors;
    sectorsInVolume = cryptoInfo->VolumeSize.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR;
    if (endEncryptedArea > (startSector + sectorsInVolume)) {
    	CS_DEBUG((D_INFO, L"inconsistent volume information: startSector: 0x%x, enc length: 0x%x, vol size: 0x%x\n",
    			startSector, cryptoInfo->EncryptedAreaLength.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR,
    			sectorsInVolume));
    	return EFI_VOLUME_CORRUPTED;
    }
    if (encrypt) {
    	source = childBlockIo;
    	dest = parentBlockIo;
    	lba = endEncryptedArea;
        numberSectors = sectorsInVolume - numberEncryptedSectors;
    	CS_DEBUG((D_INFO, L"need to encrypt 0x%x sectors starting at LBA 0x%x\n",
    			numberSectors, lba));
    } else {
    	source = parentBlockIo;
    	dest = childBlockIo;
    	if (numberEncryptedSectors < bufferSectors) {
    		bufferSectors = numberEncryptedSectors;
    	}
    	lba = startSector + numberEncryptedSectors - bufferSectors;
        numberSectors = numberEncryptedSectors;
    	CS_DEBUG((D_INFO, L"need to decrypt 0x%x sectors starting at LBA 0x%x\n",
    			numberSectors, lba));
    }

	reset_input(SystemTable->ConIn);	/* ignore the return code */

    buffer = AllocatePool(bufferSize);
    if (buffer != NULL) {
    	UINTN bytesToRead = bufferSectors << TC_LB_SIZE_BIT_SHIFT_DIVISOR;

    	while (numberSectors > 0) {

    		/* read from source */
        	error = uefi_call_wrapper(source->ReadBlocks, 5,
        			source, source->Media->MediaId, lba, bytesToRead, buffer);
        	if (EFI_ERROR(error)) {
            	CS_DEBUG((D_ERROR, L"unable to read 0x%x byte at LBA 0x%lx from media 0x%x: %r\n",
            			bytesToRead, lba, source->Media->MediaId, error));
            	break;
        	}
#if 1
        	/* write to destination */
        	error = uefi_call_wrapper(dest->WriteBlocks, 5,
        			dest, dest->Media->MediaId, lba, bytesToRead, buffer);
        	if (EFI_ERROR(error)) {
            	CS_DEBUG((D_ERROR, L"unable to write 0x%x byte at LBA 0x%lx to media 0x%x: %r\n",
            			bytesToRead, lba, dest->Media->MediaId, error));
            	break;
        	}
#endif

        	// CS_DEBUG((D_INFO, L"crypted 0x%x byte at LBA 0x%lx\n", bytesToRead, lba));

        	if (encrypt) {
            	lba += bufferSectors;
        	}

        	numberSectors -= bufferSectors;
    		if (numberSectors < bufferSectors) {
        		bufferSectors = numberSectors;
        		bytesToRead = bufferSectors << TC_LB_SIZE_BIT_SHIFT_DIVISOR;
    		}

        	if (encrypt == FALSE) {
            	lba -= bufferSectors;
        	}

        	if (sectorsInVolume > 0) {
        		if (encrypt) {
        			UINT64 x = 1000 * (lba - startSector + bufferSectors);
        			__div64_32(&x, sectorsInVolume);	/* x = 1000 * (lba - startSector + bufferSectors) / sectorsInVolume */
        			dump_per_cent(SystemTable->ConOut, x);
        		} else {
        			UINT64 x = 1000 * (lba - startSector);
        			__div64_32(&x, sectorsInVolume);	/* x = 1000 * (lba - startSector) / sectorsInVolume */
					dump_per_cent(SystemTable->ConOut, 1000 - x);
        		}
        	}

        	if (check_for_ESC(SystemTable->ConIn)) {
            	CS_DEBUG((D_INFO, L"ESC key detected... stopping...\n", lba)); CS_DEBUG_SLEEP(3);
        		break;
        	}
    	}
    	FreePool(buffer);
    } else {
    	CS_DEBUG((D_ERROR, L"unable to allocate memory for cipher buffer\n")); CS_DEBUG_SLEEP(3);
    	error = EFI_OUT_OF_RESOURCES;
    }

	if (!EFI_ERROR(error)) {
		/* update volume header and write it back to media */
		lba += (cryptoInfo->EncryptedAreaStart.Value >> TC_LB_SIZE_BIT_SHIFT_DIVISOR);
		error = update_blocks_in_volume_header((encrypt) ? lba : lba + bufferSectors, cryptoInfo, encrypt);
	}

    return error;
}

/*
 *	\brief	performs the media encryption/decryption triggered by the service menu
 *
 *  Based on the given flag "encrypt", the function encrypts or decrypts the blocks of the
 *  media. The function performs the following steps:
 *  - ask for user password and decrypt the volume header
 *  - start and connect the CS crypto driver to the given controller
 *  - get the handles of the parent controller and the child controller (the child is created
 *    by the connecetd CS crypto driver)
 *  - open Block IO protocol interfaces from both parent and child controllers
 *  - perform the encryption of decryption using these protocol interfaces
 *
 *	\param	ImageHandle		image handle
 *	\param	SystemTable		system table
 *	\param	options			options as defined by the user (taken from options file)
 *	\param	encrypt			flag indicating whether the media needs to be encryped or decrypted
 *	\param	user_decision	buffer to define the next activity after the function returns
 *	\param	passwd			buffer for the user password
 *
 *	\return		the success state of the function
 */
EFI_STATUS encrypt_decrypt_media(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable,
		IN const struct cs_option_data *options, IN BOOLEAN encrypt,
		OUT enum cs_enum_user_decision *user_decision, OUT Password *passwd) {
    EFI_STATUS error;
    EFI_HANDLE ParentHandle, ChildHandle;
    EFI_BLOCK_IO *parentBlockIo, *childBlockIo;

    ASSERT(user_decision != NULL);

    *user_decision = CS_UI_SERVICE_MENU;

    error = check_user_password(SystemTable, options, user_decision, passwd);
	if (EFI_ERROR(error)) {
		CS_DEBUG((D_ERROR, L"unable to verify user password (%r)\n", error));
		return error;
	}
	if (*user_decision == CS_UI_ESC_PRESSED) {
		/* ESC key pressed */
		*user_decision = CS_UI_SERVICE_MENU;
		return EFI_SUCCESS;	/* user refused */
	}

	if (check_really_do(SystemTable->ConIn, SystemTable->ConOut) == FALSE) {
		return EFI_SUCCESS;	/* user refused */
	}

	error = start_connect_fake_crypto_driver(ImageHandle);
	if (EFI_ERROR(error)) {
		CS_DEBUG((D_ERROR, L"Unable to start the crypto driver: %r\n", error));
	    return error;
	}

	ParentHandle = get_boot_partition_handle();
	ASSERT(ParentHandle != NULL);

	error = get_cs_child(ParentHandle, &ChildHandle);
	if (EFI_ERROR(error)) {
		CS_DEBUG((D_ERROR, L"Unable to get child handle of boot partition: %r\n", error));
	    return error;
	}
	ASSERT(ChildHandle != NULL);

	error = open_blockio_protocols(ParentHandle, ChildHandle, &parentBlockIo, &childBlockIo);
	if (!EFI_ERROR(error)) {
	    EFI_STATUS error2;

	    /* output line feed(s) before the progress bar */
	    if (!options->flags.silent) {
	        error = uefi_call_wrapper(SystemTable->ConOut->OutputString, 2, SystemTable->ConOut, L"\r\n\n");
	    }

	    /* the loop to encrypt/decrypt the entire media (until user interrupt) */
	    error = do_encrypt_decrypt_media(SystemTable, parentBlockIo, childBlockIo, encrypt);

		error2 = close_blockio_protocol(ParentHandle, parentBlockIo);
		if (EFI_ERROR(error2)) {
	    	CS_DEBUG((D_ERROR, L"unable to close parent BlockIO protocol: %r\n", error2));
		}
		error2 = close_blockio_protocol(ChildHandle, childBlockIo);
		if (EFI_ERROR(error2)) {
	    	CS_DEBUG((D_ERROR, L"unable to close child BlockIO protocol: %r\n", error2));
		}
	    *user_decision = CS_UI_REBOOT;
	}

    return error;
}

