using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace App.Services
{
    public interface IConfigService
    {
        Config Load();
    }

    public class Config
    {
        public string Host { get; set; }
        public int Port { get; set; }
    }

    public class ConfigService : IConfigService
    {
        public Config Load()
        {
            return new Config { Host = "localhost", Port = 8080 };
        }
    }

    public enum Status { OK, Error, Pending }
}
