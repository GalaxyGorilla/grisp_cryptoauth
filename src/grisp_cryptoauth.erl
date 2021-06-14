-module(grisp_cryptoauth).

%% Main API
-export([sign/2,
         sign/3,
         verify/3,
         verify/4,
         check_device/0,
         check_device/1,
         device_info/0,
         device_info/1]).

-define(PRIMARY_PRIVATE_KEY, 0).
-define(SECONDARY_PRIVATE_KEY_1, 2).
-define(SECONDARY_PRIVATE_KEY_2, 3).
-define(SECONDARY_PRIVATE_KEY_3, 4).

-define(APP, grisp_cryptoauth).
-define(DEFAULT_DEVICE, 'ATECC608B').
-define(VALID_DEVICES,
        ['ATECC508A', 'ATECC608A', 'ATECC608B']).
-define(DEFAULT_CONFIG,
        #{type => ?DEFAULT_DEVICE,
          i2c_bus => 1,
          i2c_address => 16#6C}).


%% ---------------
%% Main API
%% ---------------

sign(PrivKey, Msg) ->
    sign(PrivKey, Msg, #{}).

sign(primary, Msg, Config) ->
    do_sign(?PRIMARY_PRIVATE_KEY, Msg, Config);
sign(secondary_1, Msg, Config) ->
    do_sign(?SECONDARY_PRIVATE_KEY_1, Msg, Config);
sign(secondary_2, Msg, Config) ->
    do_sign(?SECONDARY_PRIVATE_KEY_2, Msg, Config);
sign(secondary_3, Msg, Config) ->
    do_sign(?SECONDARY_PRIVATE_KEY_3, Msg, Config).


verify(PubKey, Msg, Sig) ->
    verify(PubKey, Msg, Sig, #{}).

verify(primary, Msg, Sig, Config) ->
    do_verify(?PRIMARY_PRIVATE_KEY, Msg, Sig, Config);
verify(secondary_1, Msg, Sig, Config) ->
    do_verify(?SECONDARY_PRIVATE_KEY_1, Msg, Sig, Config);
verify(secondary_2, Msg, Sig, Config) ->
    do_verify(?SECONDARY_PRIVATE_KEY_2, Msg, Sig, Config);
verify(secondary_3, Msg, Sig, Config) ->
    do_verify(?SECONDARY_PRIVATE_KEY_3, Msg, Sig, Config).


check_device() ->
    check_device(#{}).

check_device(Config) ->
    case grisp_cryptoauth_nif:device_info(Config) of
        {ok, DeviceType} ->
            case lists:member(DeviceType, ?VALID_DEVICES) of
                true ->
                    ok;
                false ->
                    {error, invalid_device}
            end;
        Error ->
            Error
    end.


device_info() ->
    device_info(#{}).

device_info(Config) ->
    BuiltConfig = build_config(Config),
    case check_device(BuiltConfig) of
        ok ->
            Info = generate_device_info(BuiltConfig),
            io:format("~s", [Info]);
        Error ->
            Error
    end.


%% ---------------
%% Config handling
%% ---------------

validate_config(Config) ->
    lists:member(maps:get(type, Config, ?DEFAULT_DEVICE), ?VALID_DEVICES).

default_config() ->
    maps:merge(?DEFAULT_CONFIG, application:get_env(?APP, device, #{})).

build_config(Config) ->
    MergedConfig = maps:merge(default_config(), Config),
    case validate_config(MergedConfig) of
        true ->
            MergedConfig;
        false ->
            exit({badarg, invalid_config})
    end.


%% ---------------
%% Helpers
%% ---------------

do_sign(SlotIdx, Msg, Config) ->
    grisp_cryptoauth_nif:sign(build_config(Config), SlotIdx, crypto:hash(sha256, Msg)).

do_verify(SlotIdx, Msg, Sig, Config) ->
    BuiltConfig = build_config(Config),
    case grisp_cryptoauth_nif:gen_public_key(BuiltConfig, SlotIdx) of
        {ok, PubKey} ->
            grisp_cryptoauth_nif:verify_extern(BuiltConfig, PubKey, crypto:hash(sha256, Msg), Sig);
        Error ->
            Error
    end.

generate_device_info(Config) ->
    {ok, DeviceType} = grisp_cryptoauth_nif:device_info(Config),
    {ok, SerialNumber} = grisp_cryptoauth_nif:serial_number(Config),
    {ok, IsConfigLocked} = grisp_cryptoauth_nif:config_locked(Config),
    {ok, IsDataLocked} = grisp_cryptoauth_nif:data_locked(Config),
    Header = "GRiSP2 Secure Element",
    Sep = "=====================",
    DeviceTypeText = ["Type: ", atom_to_binary(DeviceType, latin1)],
    SerialNumberText = ["Serial Number: ", atom_to_binary(SerialNumber, latin1)],
    ConfigLockedText = ["Config Locked: ", atom_to_binary(IsConfigLocked, latin1)],
    DataLockedText = ["Data Locked: ", atom_to_binary(IsDataLocked, latin1)],
    io:format("~n~s~n~s~n~s~n~s~n~s~n~s",
              [Header, Sep, DeviceTypeText, SerialNumberText, ConfigLockedText, DataLockedText]).
