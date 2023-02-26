/*
  mujson

  a minimal parser reading json-like strings.

  (c) 2021 Timo Kaluza

*/

#include <stdexcept>
#include "mujson.h"

namespace neui
{
  using std::string;
  using std::vector;

  class Parser
  {
  public:    
    static std::vector<mujson::item_t> doparse(const char* s);
    enum class state_t
    {
      sigma,
      inclause,
      key,
      separator,
      valuestart,
      value,
      nextitem,
      error
    };
  private:
    class State
    {
      friend class Parser;
      state_t state = state_t::sigma;
      bool inQuotes = false;
      string key, value;
      vector<mujson::item_t> result;
    };
  };

  std::vector<mujson::item_t> Parser::doparse(const char* s)
  {
    State self;

    static const char* err_nocurlybraces = "string does not start with curly braces";
    static const char* err_unexpected_end = "unexpected string termination";
    static const char* err_unbalanced_quotes = "unbalanced quotes";
    static const char* err_unexpected_quotes = "unexpected quotes";
    static const char* err_invalid_chars_key = "key can only contain alphanumerics";
    static const char* err_expected_colon = "expected ':' between key and value";
    static const char* err_betweenitems_or_end = "expected ',' or '}'";

    using parse_error = std::runtime_error;

    bool finished = false;
    while (!finished)
    {
      switch (self.state)
      {
      case state_t::sigma:
        switch (*s++)
        {
        case ' ':
          break;
        case '{':
          self.state = state_t::inclause;
          break;
        default:
          throw parse_error(err_nocurlybraces);
        }
        break;
      case state_t::inclause:
      {
        auto c = *s++;
        switch (c)
        {
        case 0:
          throw parse_error(err_unexpected_end);
          break;
        case '\"':
          self.inQuotes = true;
          self.state = state_t::key;
          break;
        default:
          if (isalnum(c))
          {
            self.key.push_back(c);
            self.state = state_t::key;
          }
          else
          {
            throw parse_error(err_invalid_chars_key);
          }
          break;
        }
      }
      break;
      case state_t::key:
      {
        auto c = *s++;
        switch (c)
        {
        case 0:
          throw parse_error(err_unexpected_end);
          break;
        case '\"':
          if (self.inQuotes)
          {
            self.inQuotes = false;
            self.state = state_t::separator;
          }
          else
          {
            throw parse_error(err_unexpected_quotes);
          }
          break;
        case ':':
          if (self.inQuotes)
          {
            throw parse_error(err_unbalanced_quotes);
          }
          self.state = state_t::valuestart;
          break;
        default:
          if (std::isalnum(c))
          {
            self.key.push_back(c);
          }
          else
          {
            throw parse_error(err_invalid_chars_key);
          }
          break;
        }
      }
      break;
      case state_t::separator:
        switch (*s++)
        {
        case ':':
          self.state = state_t::valuestart;
          break;
        default:
          throw parse_error(err_expected_colon);
          break;
        }
        break;
      case state_t::valuestart:
      case state_t::value:
      {
        auto c = *s++;
        switch (c)
        {
        case 0:
          // terminated?
          throw parse_error(err_unexpected_end);
          break;
        case '\"':
          if (self.state == state_t::valuestart)
          {
            self.inQuotes = true;
            self.state = state_t::value;
          }
          else
          {
            if (self.inQuotes)
            {
              self.inQuotes = false;
              self.result.push_back({ self.key,self.value });
              self.key.clear();
              self.value.clear();
              // CREATE ITEM HERE
              self.state = state_t::nextitem;
            }
            else
            {
              throw parse_error(err_unexpected_quotes);
            }

          }
          break;
        case ',':
          if (self.inQuotes)
          {
            self.value.push_back(c);
          }
          else
          {
            self.result.push_back({ self.key,self.value });
            self.key.clear();
            self.value.clear();
            self.state = state_t::inclause;
          }
          break;
        case '}':
          if (self.inQuotes)
          {
            throw parse_error(err_unbalanced_quotes);
          }
          self.result.push_back({ self.key,self.value });
          self.key.clear();
          self.value.clear();
          self.state = state_t::sigma;
          // finished
          finished = true;
          break;
        default:
          self.value.push_back(c);
          break;
        }
      }
      break;
      case state_t::nextitem:
        switch (*s++)
        {
        case ',':
          self.state = state_t::inclause;
          break;
        case '}':
          self.state = state_t::sigma;
          // finished
          finished = true;
          break;
        default:
          throw parse_error(err_betweenitems_or_end);
          break;
        }
        break;
      case state_t::error:
        throw parse_error("mujson falls into error state");
        break;
      }
    }
    return self.result;
  }

  std::vector<mujson::item_t> mujson::parse(const char* s)
  {
    return Parser::doparse(s);
  }
}