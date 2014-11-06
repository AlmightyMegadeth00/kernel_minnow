	

    #!/bin/bash
     
    ##[[ `diff arch/arm/configs/minnow_defconfig .config ` ]] && \
    ##      { echo "Unmatched defconfig!"; exit -1; }
    #
    # Variables
    #
    
    DATE_START=$(date +"%s")
    # Output directory.. made in advance
    CWM_MOVE="media/primedirective/Storage/kernel_output/minnow"
    RPWD=$PWD
    # Toolchain location
    #TCHN="/media/primedirective/Storage/kernel/toolchains/4.7.x-google"
    TCHN="/media/primedirective/Storage/vanir3/prebuilts/gcc/linux-x86/arm/linaro/linaro-4.9-cortex-a15"

    make clean; sleep 3; make distclean; sleep 3
    echo ""
    rm -rfv .config; rm -rfv .config.old
    echo ""
     
    sed -i s/CONFIG_LOCALVERSION=\".*\"/CONFIG_LOCALVERSION=\"~vanir_minnow_${1}\"/ arch/arm/configs/minnow_defconfig
     
    make CROSS_COMPILE=$TCHN/bin/arm-eabi- ARCH=arm minnow_defconfig
    make CROSS_COMPILE=$TCHN/bin/arm-eabi- ARCH=arm -j4
     
    #sed -i s/CONFIG_LOCALVERSION=\".*\"/CONFIG_LOCALVERSION=\"\"/ .config
    #cp .config arch/arm/configs/minnow_defconfig
     
    echo ""
    echo "__________________________"
    echo " done making kernel"
    echo "__________________________"
    echo ""
     
    zipfile="vanir_kernel_minnow_v${1}.zip"
    if [ ! $4 ]; then
            rm -f /tmp/*.img
            echo "making zip file"
        cp -vr arch/arm/boot/zImage ../anykernel_minnow/
        find . -name \*.ko -exec cp '{}' ../anykernel_minnow/system/lib/modules/ ';'
        cd ../anykernel_minnow/
            rm -f *.zip
            zip -r $zipfile *
            rm -f /tmp/*.zip
            cp *.zip /$CWM_MOVE
    fi
    if [[ $1 == *exp* ]]; then
        if [[ $1 == *bm* ]]; then
                mf="44latestbigmem"
        else
                mf="44latestexp"
              fi
    else
      mf="44latest"
    fi
    
    cd ../kernel_moto360
    
    #
    # PRINT BUILDING COMPILE-TIME
    #
    echo ""
    DATE_END=$(date +"%s")
    DIFF=$(($DATE_END - $DATE_START))
    echo "Build completed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
    echo ""
    #increment_version $version > $RPWD/versio


