#!/bin/bash

penalty=$1
model_folder=$2

HDF5_INPUT="/raid/nanopore/shubham/20190629_nanopore_data/raw_signal/raw_signal_6.hdf5"
DIRNAME=data_real_exp/8_5_$model_folder"_"$penalty"_full"/
mkdir -p $DIRNAME
python3 -u generate_decoded_lists.py \
--hdf_file $HDF5_INPUT \
--out_prefix $DIRNAME/list \
--read_id_file data_real_exp/read_id_lists/read_id_8_5.txt \
--info_file $DIRNAME/info.txt \
--mem_conv 8 \
--msg_len 180 \
--rate_conv 5 \
--list_size 8 \
--start_barcode GCTAGTACGCGAACAGAGTGCAGTA \
--end_barcode ACAGATGCAGTAATTCTCACGAACT \
--num_threads 10 \
--barcode_extend_penalty $penalty \
--bonito_model_path bonito_models/$model_folder
# comment out bonito_model_path line for default model